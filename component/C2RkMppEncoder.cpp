//#define LOG_NDEBUG 0
#define LOG_TAG "C2RkMppEncoder"
#include <log/log.h>

// libmpp requires this.
#undef MODULE_TAG
#define MODULE_TAG "C2RkMppEncoder"

#include <string.h>

#include <memory>
#include <vector>

#include <Codec2Mapper.h>
#include <C2AllocatorGralloc.h>
#include <C2RkMppEncoder.hpp>

#include <drm/android/gralloc_handle.h>
#include <media/stagefright/MediaCodecConstants.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/GraphicBufferAllocator.h>

#include <csc_gles.h>

#include <mpp/mpp_buffer.h>
#include <mpp/mpp_meta.h>
#include <mpp/mpp_packet.h>
#include <mpp/osal/mpp_platform.h>
#include <mpp/rk_venc_cfg.h>
#include <mpp/rk_venc_rc.h>

#include <vendor_rockchip.h>

namespace codec2::rkmpp {

void ParseGop(
	const C2StreamGopTuning::output &gop,
	uint32_t *syncInterval, uint32_t *iInterval, uint32_t *maxBframes)
{
	uint32_t syncInt = 1;
	uint32_t iInt = 1;

	for (size_t i = 0; i < gop.flexCount(); ++i) {
		const C2GopLayerStruct &layer = gop.m.values[i];
		if (layer.count == UINT32_MAX) {
			syncInt = 0;
		} else if (syncInt <= UINT32_MAX / (layer.count + 1)) {
			syncInt *= (layer.count + 1);
		}
		if ((layer.type_ & I_FRAME) == 0) {
			if (layer.count == UINT32_MAX) {
				iInt = 0;
			} else if (iInt <= UINT32_MAX / (layer.count + 1)) {
				iInt *= (layer.count + 1);
			}
		}
		if (layer.type_ == C2Config::picture_type_t(P_FRAME | B_FRAME) && maxBframes) {
			*maxBframes = layer.count;
		}
	}

	if (syncInterval) {
		*syncInterval = syncInt;
	}
	if (iInterval) {
		*iInterval = iInt;
	}
}

C2RkMppEncoder::C2RkMppEncoder(
	const char *name,
	const char *mime,
	c2_node_id_t id,
	const std::shared_ptr<IntfImpl> &intfImpl)
	: C2RkMppComponent(std::make_shared<C2RkMppComponentInterface<IntfImpl>>(name, id, intfImpl)),
	mName(name),
	mMime(mime),
	mIntf(intfImpl),
	mDmaMem(nullptr),
	mMppCtx(nullptr),
	mMppMpi(nullptr),
	mEncCfg(nullptr),
	mCodingType(MPP_VIDEO_CodingUnused),
	mInputMppFmt(MPP_FMT_YUV420SP),
	mChipType(rk_chip_get_type()),
	mStarted(false),
	mSpsPpsHeaderReceived(false),
	mSawInputEOS(false),
	mOutputEOS(false),
	mSignalledError(false),
	mHorStride(0),
	mVerStride(0),
	mCurLayerCount(0),
	mInputCount(0)
{
	mCodingType = (MppCodingType) mppCodingFromMime(mime);
	if (mCodingType == MPP_VIDEO_CodingUnused) {
		ALOGE("failed to get coding from name %s", name);
	}
}

C2RkMppEncoder::~C2RkMppEncoder(void)
{
	C2RkMppEncoder::onRelease();
}

c2_status_t C2RkMppEncoder::onInit(void)
{
	ALOGV("enter onInit");

	return C2_OK;
}

c2_status_t C2RkMppEncoder::onStop(void)
{
	ALOGV("enter onStop");
	return C2_OK;
}

void C2RkMppEncoder::onReset(void)
{
	ALOGV("enter onReset");
}

void C2RkMppEncoder::onRelease(void)
{
	ALOGV("enter onRelease");

	if (!mStarted)
	    return;

	if (mEncCfg) {
	    mpp_enc_cfg_deinit(mEncCfg);
	    mEncCfg = nullptr;
	}

	if (mMppCtx){
	    mpp_destroy(mMppCtx);
	    mMppCtx = nullptr;
	}

	if (mDmaMem != nullptr) {
	    GraphicBufferAllocator::get().free((buffer_handle_t)mDmaMem->handler);
	    free(mDmaMem);
	    mDmaMem = nullptr;
	}

	mStarted = false;
	mSpsPpsHeaderReceived = false;
	mSawInputEOS = false;
	mOutputEOS = false;
	mSignalledError = false;
}

c2_status_t C2RkMppEncoder::onFlush_sm(void)
{
	ALOGV("enter onFlush_sm");
	return C2_OK;
}

c2_status_t C2RkMppEncoder::setupBaseCodec(void)
{
	/* default stride */
	mHorStride = C2_ALIGN(mSize->width, 16);
	if (mCodingType == MPP_VIDEO_CodingVP8) {
		mVerStride = C2_ALIGN(mSize->height, 16);
	} else {
		mVerStride = C2_ALIGN(mSize->height, 8);
	}

	ALOGI("setupBaseCodec: coding %s w %d h %d hor %d ver %d",
		toStr_Coding(mCodingType), mSize->width, mSize->height, mHorStride, mVerStride);
	
	mpp_enc_cfg_set_s32(mEncCfg, "codec:type", mCodingType);
	mpp_enc_cfg_set_s32(mEncCfg, "vp8:disable_ivf", 1);
	
	mpp_enc_cfg_set_s32(mEncCfg, "prep:width", mSize->width);
	mpp_enc_cfg_set_s32(mEncCfg, "prep:height", mSize->height);
	mpp_enc_cfg_set_s32(mEncCfg, "prep:hor_stride", mHorStride);
	mpp_enc_cfg_set_s32(mEncCfg, "prep:ver_stride", mVerStride);
	mpp_enc_cfg_set_s32(mEncCfg, "prep:format", MPP_FMT_YUV420SP);

	return C2_OK;
}

c2_status_t C2RkMppEncoder::setupBitRate(void)
{
	uint32_t bitrate = 0;
	uint32_t bitrateMode = 0;

	IntfImpl::Lock lock = mIntf->lock();

	bitrate = mIntf->getBitrate_l()->value;
	bitrateMode = mIntf->getBitrateMode_l();

	ALOGI("setupBitRate: mode %s bitrate %d",
		toStr_BitrateMode(bitrateMode), bitrate);
	
	mpp_enc_cfg_set_s32(mEncCfg, "rc:bps_target", bitrate);
	switch (bitrateMode) {
	case MPP_ENC_RC_MODE_CBR: {
		/* CBR mode has narrow bound */
		mpp_enc_cfg_set_s32(mEncCfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
		mpp_enc_cfg_set_s32(mEncCfg, "rc:bps_max", bitrate * 17 / 16);
		mpp_enc_cfg_set_s32(mEncCfg, "rc:bps_min", bitrate * 15 / 16);
	} break;
	case MPP_ENC_RC_MODE_VBR: {
		/* VBR mode has wide bound */
		mpp_enc_cfg_set_s32(mEncCfg, "rc:mode", MPP_ENC_RC_MODE_VBR);
		mpp_enc_cfg_set_s32(mEncCfg, "rc:bps_max", bitrate * 17 / 16);
		mpp_enc_cfg_set_s32(mEncCfg, "rc:bps_min", bitrate * 1 / 16);
	} break;
	case MPP_ENC_RC_MODE_FIXQP: {
		/* FIXQP mode */
		mpp_enc_cfg_set_s32(mEncCfg, "rc:mode", MPP_ENC_RC_MODE_FIXQP);
		mpp_enc_cfg_set_s32(mEncCfg, "rc:bps_max", bitrate * 17 / 16);
		mpp_enc_cfg_set_s32(mEncCfg, "rc:bps_min", bitrate * 15 / 16);
	} break;
	default: {
		/* default use CBR mode */
		mpp_enc_cfg_set_s32(mEncCfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
		mpp_enc_cfg_set_s32(mEncCfg, "rc:bps_max", bitrate * 17 / 16);
		mpp_enc_cfg_set_s32(mEncCfg, "rc:bps_min", bitrate * 15 / 16);
	} break;
	}

	return C2_OK;
};

c2_status_t C2RkMppEncoder::setupFrameRate(void)
{
	float frameRate = 0.0f;
	uint32_t idrInterval = 0;
	int32_t gop = 0;

	IntfImpl::Lock lock = mIntf->lock();

	std::shared_ptr<C2StreamGopTuning::output> c2Gop = mIntf->getGop_l();
	std::shared_ptr<C2StreamFrameRateInfo::output> c2FrameRate
		= mIntf->getFrameRate_l();

	idrInterval = mIntf->getSyncFramePeriod_l();
	frameRate = c2FrameRate->value;

	if (frameRate == 1) {
		// set default frameRate 30
		frameRate = 30;
	}

	if (c2Gop && c2Gop->flexCount() > 0) {
		uint32_t syncInterval = 30;
		uint32_t iInterval = 0;
		uint32_t maxBframes = 0;
		
		ParseGop(*c2Gop, &syncInterval, &iInterval, &maxBframes);
		if (syncInterval > 0) {
			ALOGI("updating IDR interval: %d -> %d", idrInterval, syncInterval);
			idrInterval = syncInterval;
		}
	}

	ALOGI("setupFrameRate: framerate %.2f gop %u", frameRate, idrInterval);

	gop = (idrInterval < INT_MAX) ? idrInterval : 0;

	mpp_enc_cfg_set_s32(mEncCfg, "rc:gop", gop);

	/* fix input / output frame rate */
	mpp_enc_cfg_set_s32(mEncCfg, "rc:fps_in_flex", 0);
	mpp_enc_cfg_set_s32(mEncCfg, "rc:fps_in_num", frameRate);
	mpp_enc_cfg_set_s32(mEncCfg, "rc:fps_in_denorm", 1);
	mpp_enc_cfg_set_s32(mEncCfg, "rc:fps_out_flex", 0);
	mpp_enc_cfg_set_s32(mEncCfg, "rc:fps_out_num", frameRate);
	mpp_enc_cfg_set_s32(mEncCfg, "rc:fps_out_denorm", 1);

	return C2_OK;
}

c2_status_t C2RkMppEncoder::setupEncCfg(void)
{
	MPP_RET err = MPP_OK;

	err = mpp_enc_cfg_init(&mEncCfg);
	if (err != MPP_OK) {
		ALOGE("failed to get enc_cfg, err %d", err);
		return C2_CORRUPTED;
	}

	err = mMppMpi->control(mMppCtx, MPP_ENC_GET_CFG, mEncCfg);
	if (err != MPP_OK) {
		ALOGE("failed to get codec cfg, err %d", err);
		return C2_CORRUPTED;
	}

	/* Video control Set Base Codec */
	setupBaseCodec();

	/* Video control Set Rotation */
	{
		IntfImpl::Lock lock = mIntf->lock();
		std::shared_ptr<C2StreamRotationInfo::output> c2Rotation
			= mIntf->getRotation_l();
		int32_t degrees = c2Rotation->value;

		if (degrees > 0) {
			ALOGI("setupRotation: degrees %d", degrees);
			
			switch (degrees) {
			case 90:
				mpp_enc_cfg_set_s32(mEncCfg, "prep:rotation", MPP_ENC_ROT_90);
				break;
			case 180:
				mpp_enc_cfg_set_s32(mEncCfg, "prep:rotation", MPP_ENC_ROT_180);
				break;
			case 270:
				mpp_enc_cfg_set_s32(mEncCfg, "prep:rotation", MPP_ENC_ROT_270);
				break;
			default:
				ALOGW("We only support 0,90,180,270 degree rotation");
				break;
			}
		}
	}

	/* Video control Set FrameRates and gop */
	setupFrameRate();

	/* Video control Set Bitrate */
	setupBitRate();

	/* Video control Set QP */
	{
		int32_t defaultIMin = 0, defaultIMax = 0;
		int32_t defaultPMin = 0, defaultPMax = 0;
		int32_t qpInit = -1, fixQPMode /* const qp mode */ = 0;

		if (mCodingType == MPP_VIDEO_CodingVP8) {
			defaultIMin = defaultPMin = 0;
			defaultIMax = defaultPMax = 127;
		} else {
			/* the quality of h264/265 range from 1~51 */
			defaultIMin = defaultPMin = 1;
			defaultIMax = 51;
			// TODO: CTS testEncoderQualityAVCCBR 49
			defaultPMax = 49;
		}
	
		int32_t iMin = defaultIMin, iMax = defaultIMax;
		int32_t pMin = defaultPMin, pMax = defaultPMax;
	
		IntfImpl::Lock lock = mIntf->lock();
	
		std::shared_ptr<C2StreamPictureQuantizationTuning::output> qp =
			mIntf->getPictureQuantization_l();
		fixQPMode = (mIntf->getBitrateMode_l() == MPP_ENC_RC_MODE_FIXQP) ? 1 : 0;
	
		for (size_t i = 0; i < qp->flexCount(); ++i) {
			const C2PictureQuantizationStruct &layer = qp->m.values[i];
			
			if (layer.type_ == C2Config::picture_type_t(I_FRAME)) {
				iMax = layer.max;
				iMin = layer.min;
				ALOGI("PictureQuanlitySetter: iMin %d iMax %d", iMin, iMax);
			} else if (layer.type_ == C2Config::picture_type_t(P_FRAME)) {
				pMax = layer.max;
				pMin = layer.min;
				ALOGI("PictureQuanlitySetter: pMin %d pMax %d", pMin, pMax);
			}
		}
	
		iMax = std::clamp(iMax, defaultIMin, defaultIMax);
		iMin = std::clamp(iMin, defaultIMin, defaultIMax);
		pMax = std::clamp(pMax, defaultPMin, defaultPMax);
		pMin = std::clamp(pMin, defaultPMin, defaultPMax);
	
		if (fixQPMode) {
			/* use const qp for p-frame in FIXQP mode */
			pMax = pMin = qpInit;
		}
	
		ALOGI("setupQp: qpInit %d i %d-%d p %d-%d", qpInit, iMin, iMax, pMin, pMax);
	
		switch (mCodingType) {
		case MPP_VIDEO_CodingAVC:
			mpp_enc_cfg_set_s32(mEncCfg, "h264:cb_qp_offset", 0);
			mpp_enc_cfg_set_s32(mEncCfg, "h264:cr_qp_offset", 0);
			[[fallthrough]];
		case MPP_VIDEO_CodingHEVC: {
			/*
			 * disable mb_rc for vepu, this cfg does not apply to rkvenc.
			 * since the vepu has pool performance, mb_rc will cause mosaic.
			 */
			mpp_enc_cfg_set_s32(mEncCfg, "hw:mb_rc_disable", 1);
			
			mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_min", pMin);
			mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_max", pMax);
			mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_min_i", iMin);
			mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_max_i", iMax);
			mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_init", qpInit);
			mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_ip", 2);
		} break;
		case MPP_VIDEO_CodingVP8: {
			mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_min", pMin);
			mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_max", pMax);
			mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_min_i", iMin);
			mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_max_i", iMax);
			mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_init", qpInit);
			mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_ip", 6);
		} break;
		default: {
			ALOGE("setupQp: unsupport coding type %d", mCodingType);
			break;
		}
		}
	}

	/* Video control Set VUI params */
	{
		ColorAspects sfAspects;
		int32_t primaries, transfer, matrixCoeffs;
		bool range;

		IntfImpl::Lock lock = mIntf->lock();

		std::shared_ptr<C2StreamColorAspectsInfo::output> colorAspects
			= mIntf->getCodedColorAspects_l();

		if (!C2Mapper::map(colorAspects->primaries, &sfAspects.mPrimaries)) {
			sfAspects.mPrimaries = android::ColorAspects::PrimariesUnspecified;
		}
		if (!C2Mapper::map(colorAspects->range, &sfAspects.mRange)) {
			sfAspects.mRange = android::ColorAspects::RangeUnspecified;
		}
		if (!C2Mapper::map(colorAspects->matrix, &sfAspects.mMatrixCoeffs)) {
			sfAspects.mMatrixCoeffs = android::ColorAspects::MatrixUnspecified;
		}
		if (!C2Mapper::map(colorAspects->transfer, &sfAspects.mTransfer)) {
			sfAspects.mTransfer = android::ColorAspects::TransferUnspecified;
		}
	
		ColorUtils::convertCodecColorAspectsToIsoAspects(
			sfAspects, &primaries, &transfer,
			&matrixCoeffs, &range);
		
		ALOGI("setupVuiParams: (R:%d(%s), P:%d(%s), M:%d(%s), T:%d(%s))",
			sfAspects.mRange, asString(sfAspects.mRange),
			sfAspects.mPrimaries, asString(sfAspects.mPrimaries),
			sfAspects.mMatrixCoeffs, asString(sfAspects.mMatrixCoeffs),
			sfAspects.mTransfer, asString(sfAspects.mTransfer));
		
		mpp_enc_cfg_set_s32(mEncCfg, "prep:range", range ? 2 : 0);
		mpp_enc_cfg_set_s32(mEncCfg, "prep:colorprim", primaries);
		mpp_enc_cfg_set_s32(mEncCfg, "prep:colortrc", transfer);
		mpp_enc_cfg_set_s32(mEncCfg, "prep:colorspace", matrixCoeffs);
	}

	/* Video control Set Temporal Layers */
	{
		int32_t layerCount = 0;

		IntfImpl::Lock lock = mIntf->lock();

		std::shared_ptr<C2StreamTemporalLayeringTuning::output> layering =
		        mIntf->getTemporalLayers_l();

		layerCount = layering->m.layerCount;
		if (layerCount == 0 || layerCount == 1) {
			return C2_OK;
		}

		if (layerCount < 2 || layerCount > 4) {
			ALOGW("only support tsvc layer 2 ~ 4(%d); ignored.", layerCount);
			return C2_OK;
		}

		/*
		 * NOTE:
		 * 1. not support set bLayerCount and bitrateRatios yet.
		 *    - layering->m.bLayerCount
		 *    - layering->m.bitrateRatios
		 * 2. only support tsvc layer 2 ~ 4.
		 */

		MPP_RET err = MPP_OK;
		MppEncRefCfg ref;
		MppEncRefLtFrmCfg ltRef[4];
		MppEncRefStFrmCfg stRef[16];
		RK_S32 ltCnt = 0;
		RK_S32 stCnt = 0;

		memset(&ltRef, 0, sizeof(ltRef));
		memset(&stRef, 0, sizeof(stRef));

		mpp_enc_ref_cfg_init(&ref);

		ALOGI("setupTemporalLayers: layers %d", layerCount);

		switch (layerCount) {
		case 4: {
		// tsvc4
		//      /-> P1      /-> P3        /-> P5      /-> P7
		//     /           /             /           /
		//    //--------> P2            //--------> P6
		//   //                        //
		//  ///---------------------> P4
		// ///
		// P0 ------------------------------------------------> P8
		ltCnt = 1;

		/* set 8 frame lt-ref gap */
		ltRef[0].lt_idx        = 0;
		ltRef[0].temporal_id   = 0;
		ltRef[0].ref_mode      = REF_TO_PREV_LT_REF;
		ltRef[0].lt_gap        = 8;
		ltRef[0].lt_delay      = 0;

		stCnt = 9;
		/* set tsvc4 st-ref struct */
		/* st 0 layer 0 - ref */
		stRef[0].is_non_ref    = 0;
		stRef[0].temporal_id   = 0;
		stRef[0].ref_mode      = REF_TO_TEMPORAL_LAYER;
		stRef[0].ref_arg       = 0;
		stRef[0].repeat        = 0;
		/* st 1 layer 3 - non-ref */
		stRef[1].is_non_ref    = 1;
		stRef[1].temporal_id   = 3;
		stRef[1].ref_mode      = REF_TO_PREV_REF_FRM;
		stRef[1].ref_arg       = 0;
		stRef[1].repeat        = 0;
		/* st 2 layer 2 - ref */
		stRef[2].is_non_ref    = 0;
		stRef[2].temporal_id   = 2;
		stRef[2].ref_mode      = REF_TO_PREV_REF_FRM;
		stRef[2].ref_arg       = 0;
		stRef[2].repeat        = 0;
		/* st 3 layer 3 - non-ref */
		stRef[3].is_non_ref    = 1;
		stRef[3].temporal_id   = 3;
		stRef[3].ref_mode      = REF_TO_PREV_REF_FRM;
		stRef[3].ref_arg       = 0;
		stRef[3].repeat        = 0;
		/* st 4 layer 1 - ref */
		stRef[4].is_non_ref    = 0;
		stRef[4].temporal_id   = 1;
		stRef[4].ref_mode      = REF_TO_PREV_LT_REF;
		stRef[4].ref_arg       = 0;
		stRef[4].repeat        = 0;
		/* st 5 layer 3 - non-ref */
		stRef[5].is_non_ref    = 1;
		stRef[5].temporal_id   = 3;
		stRef[5].ref_mode      = REF_TO_PREV_REF_FRM;
		stRef[5].ref_arg       = 0;
		stRef[5].repeat        = 0;
		/* st 6 layer 2 - ref */
		stRef[6].is_non_ref    = 0;
		stRef[6].temporal_id   = 2;
		stRef[6].ref_mode      = REF_TO_PREV_REF_FRM;
		stRef[6].ref_arg       = 0;
		stRef[6].repeat        = 0;
		/* st 7 layer 3 - non-ref */
		stRef[7].is_non_ref    = 1;
		stRef[7].temporal_id   = 3;
		stRef[7].ref_mode      = REF_TO_PREV_REF_FRM;
		stRef[7].ref_arg       = 0;
		stRef[7].repeat        = 0;
		/* st 8 layer 0 - ref */
		stRef[8].is_non_ref    = 0;
		stRef[8].temporal_id   = 0;
		stRef[8].ref_mode      = REF_TO_TEMPORAL_LAYER;
		stRef[8].ref_arg       = 0;
		stRef[8].repeat        = 0;
		} break;
		case 3: {
		// tsvc3
		//     /-> P1      /-> P3
		//    /           /
		//   //--------> P2
		//  //
		// P0/---------------------> P4
		ltCnt = 0;

		stCnt = 5;
		/* set tsvc4 st-ref struct */
		/* st 0 layer 0 - ref */
		stRef[0].is_non_ref    = 0;
		stRef[0].temporal_id   = 0;
		stRef[0].ref_mode      = REF_TO_TEMPORAL_LAYER;
		stRef[0].ref_arg       = 0;
		stRef[0].repeat        = 0;
		/* st 1 layer 2 - non-ref */
		stRef[1].is_non_ref    = 1;
		stRef[1].temporal_id   = 2;
		stRef[1].ref_mode      = REF_TO_PREV_REF_FRM;
		stRef[1].ref_arg       = 0;
		stRef[1].repeat        = 0;
		/* st 2 layer 1 - ref */
		stRef[2].is_non_ref    = 0;
		stRef[2].temporal_id   = 1;
		stRef[2].ref_mode      = REF_TO_PREV_REF_FRM;
		stRef[2].ref_arg       = 0;
		stRef[2].repeat        = 0;
		/* st 3 layer 2 - non-ref */
		stRef[3].is_non_ref    = 1;
		stRef[3].temporal_id   = 2;
		stRef[3].ref_mode      = REF_TO_PREV_REF_FRM;
		stRef[3].ref_arg       = 0;
		stRef[3].repeat        = 0;
		/* st 4 layer 0 - ref */
		stRef[4].is_non_ref    = 0;
		stRef[4].temporal_id   = 0;
		stRef[4].ref_mode      = REF_TO_TEMPORAL_LAYER;
		stRef[4].ref_arg       = 0;
		stRef[4].repeat        = 0;
		} break;
		case 2: {
		// tsvc2
		//   /-> P1
		//  /
		// P0--------> P2
		ltCnt = 0;
		
		stCnt = 3;
		/* set tsvc4 st-ref struct */
		/* st 0 layer 0 - ref */
		stRef[0].is_non_ref    = 0;
		stRef[0].temporal_id   = 0;
		stRef[0].ref_mode      = REF_TO_TEMPORAL_LAYER;
		stRef[0].ref_arg       = 0;
		stRef[0].repeat        = 0;
		/* st 1 layer 2 - non-ref */
		stRef[1].is_non_ref    = 1;
		stRef[1].temporal_id   = 1;
		stRef[1].ref_mode      = REF_TO_PREV_REF_FRM;
		stRef[1].ref_arg       = 0;
		stRef[1].repeat        = 0;
		/* st 2 layer 1 - ref */
		stRef[2].is_non_ref    = 0;
		stRef[2].temporal_id   = 0;
		stRef[2].ref_mode      = REF_TO_PREV_REF_FRM;
		stRef[2].ref_arg       = 0;
		stRef[2].repeat        = 0;
		} break;
		default : {
		} break;
		}

		if (ltCnt || stCnt) {
			mpp_enc_ref_cfg_set_cfg_cnt(ref, ltCnt, stCnt);
			
			if (ltCnt)
				mpp_enc_ref_cfg_add_lt_cfg(ref, ltCnt, ltRef);
			
			if (stCnt)
				mpp_enc_ref_cfg_add_st_cfg(ref, stCnt, stRef);
			
			/* check and get dpb size */
			mpp_enc_ref_cfg_check(ref);
		}

		err = mMppMpi->control(mMppCtx, MPP_ENC_SET_REF_CFG, ref);
		if (err != MPP_OK) {
			ALOGE("setupTemporalLayers: failed to set ref cfg, err %d", err);
			return C2_CORRUPTED;
		}

		mCurLayerCount = layerCount;
	}

	/* Video control Set Prepend Header Setting */
	{
		std::shared_ptr<C2PrependHeaderModeSetting> prepend;

		IntfImpl::Lock lock = mIntf->lock();

		prepend = mIntf->getPrependHeaderMode_l();

		if (prepend->value == C2Config::PREPEND_HEADER_TO_ALL_SYNC) {
			ALOGI("setupPrependHeaderSetting: prepend sps pps to idr frames.");
			MppEncHeaderMode mode = MPP_ENC_HEADER_MODE_EACH_IDR;
			MPP_RET err = mMppMpi->control(mMppCtx, MPP_ENC_SET_HEADER_MODE, &mode);
			if (err != MPP_OK) {
				ALOGE("setupPrependHeaderSetting: failed to set mode, err %d", err);
				return C2_CORRUPTED;
			}
		}
	}

	///* Video control Set Scene Mode */
	//{
	//	IntfImpl::Lock lock = mIntf->lock();

	//	std::shared_ptr<C2StreamSceneModeInfo::input> c2Mode = mIntf->getSceneMode_l();

	//	ALOGI("setupSceneMode: scene-mode %d", c2Mode->value);

	//	/*
	//	 * scene-mode of encoder, this feature only support on rk3588
	//	 *   - 0: deault none ipc mode
	//	 *   - 1: ipc mode
	//	 */
	//	mpp_enc_cfg_set_s32(mEncCfg, "tune:scene_mode", c2Mode->value);
	//}
	///* Video control Set Slice Size */
	//{
	//	IntfImpl::Lock lock = mIntf->lock();

	//	std::shared_ptr<C2StreamSliceSizeInfo::input> c2Size = mIntf->getSliceSize_l();
			
	//	if (c2Size->value > 0) {
	//		ALOGI("setupSliceSize: slice-size %d", c2Size->value);
	//		mpp_enc_cfg_set_s32(mEncCfg, "split:mode", MPP_ENC_SPLIT_BY_BYTE);
	//		mpp_enc_cfg_set_s32(mEncCfg, "split:arg", c2Size->value);
	//	}
	//}

	///* Video control Set reenc Times */
	//{
	//	IntfImpl::Lock lock = mIntf->lock();

	//	std::shared_ptr<C2StreamReencInfo::input> reencTime = mIntf->getReencTime_l();

	//	if (reencTime->value > 0) {
	//		ALOGI("setupReencTimes: reenc-times %d", reencTime->value);
	//		mpp_enc_cfg_set_s32(mEncCfg, "prep:max_reenc_times", reencTime->value);
	//	}
	//}

	///* Video control Set Profile params */
	//{
	//	uint32_t profile, level;

	//	IntfImpl::Lock lock = mIntf->lock();

	//	profile = mIntf->getProfile_l(mCodingType);
	//	level = mIntf->getLevel_l(mCodingType);

	//	ALOGI("setupProfileParams: profile %s level %s",
	//		toStr_Profile(profile, mCodingType), toStr_Level(level, mCodingType));
		
	//	switch (mCodingType) {
	//	case MPP_VIDEO_CodingAVC : {
	//		mpp_enc_cfg_set_s32(mEncCfg, "h264:profile", profile);
	//		mpp_enc_cfg_set_s32(mEncCfg, "h264:level", level);
	//		if (profile >= MPP_H264_HIGH) {
	//			mpp_enc_cfg_set_s32(mEncCfg, "h264:cabac_en", 1);
	//			mpp_enc_cfg_set_s32(mEncCfg, "h264:cabac_idc", 0);
	//			mpp_enc_cfg_set_s32(mEncCfg, "h264:trans8x8", 1);
	//		}
	//	} break;
	//	case MPP_VIDEO_CodingHEVC : {
	//		mpp_enc_cfg_set_s32(mEncCfg, "h265:profile", profile);
	//		mpp_enc_cfg_set_s32(mEncCfg, "h265:level", level);
	//	} break;
	//	default : {
	//		ALOGE("setupProfileParams: unsupport coding type %d", mCodingType);
	//	} break;
	//	}
	//}

	err = mMppMpi->control(mMppCtx, MPP_ENC_SET_CFG, mEncCfg);
	if (err != MPP_OK) {
		ALOGE("failed to setup codec cfg, ret %d", err);
		return C2_CORRUPTED;
	} else {
		/* optional */
		MppEncSeiMode seiMode = MPP_ENC_SEI_MODE_ONE_FRAME;
		mMppMpi->control(mMppCtx, MPP_ENC_SET_SEI_CFG, &seiMode);
	}

	return C2_OK;
}

c2_status_t C2RkMppEncoder::initEncoder(void)
{
	MPP_RET err = MPP_OK;
	MppPollType timeout = MPP_POLL_BLOCK;

	ALOGD("Initializing C2 MPP encoder.");

	{
		IntfImpl::Lock lock = mIntf->lock();
		mSize = mIntf->getSize_l();
		mBitrate = mIntf->getBitrate_l();
		mFrameRate = mIntf->getFrameRate_l();
	}

	/*
	 * NOTE: We need temporary buffer to store rga nv12 output for some rgba input,
	 * since mpp can't process rgba input properly. in addition to this, alloc buffer
	 * within 4G in view of rga efficiency.
	 */
	buffer_handle_t bufferHandle;
	uint32_t stride = 0;

	uint64_t usage = (GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN);
	usage |= GRALLOC_USAGE_HW_VIDEO_ENCODER; // Add this to help gralloc-generic select the backend.

	//  allocate buffer within 4G to avoid rga2 error.
	//if (mChipType == RK_CHIP_3588 || mChipType == RK_CHIP_356X) {
	//	usage = RK_GRALLOC_USAGE_WITHIN_4G;
	//}

	status_t status = GraphicBufferAllocator::get().allocate(
			C2_ALIGN(mSize->width, 16), C2_ALIGN(mSize->height, 16),
			HAL_PIXEL_FORMAT_YCrCb_NV12 /* 0x15 */, 1u /* layer count */,
			usage, &bufferHandle, &stride, "C2RkMppEncoder");
	if (status) {
		ALOGE("failed transaction: allocate");
		goto error;
	}

	mDmaMem = (MyDmaBuffer_t *) malloc(sizeof(MyDmaBuffer_t));
	//mDmaMem->fd = C2RKGrallocOps::get()->getShareFd(bufferHandle);
	//status = GraphicBufferMapper::get();
	mDmaMem->fd = gralloc_handle(bufferHandle)->prime_fd;
	status = GraphicBufferMapper::get().getAllocationSize(bufferHandle, &mDmaMem->size);
	if (status) {
		ALOGE("failed transaction: getAllocationSize");
		goto error;
	}
	mDmaMem->handler = (void *)bufferHandle;

	ALOGI("alloc temporary DmaMem fd %d size %ld", mDmaMem->fd, mDmaMem->size);

	// create mpp and init mpp
	err = mpp_create(&mMppCtx, &mMppMpi);
	if (err != MPP_OK) {
		ALOGE("failed to mpp_create, err %d", err);
		goto error;
	}

	err = mMppMpi->control(mMppCtx, MPP_SET_OUTPUT_TIMEOUT, &timeout);
	if (err != MPP_OK) {
		ALOGE("failed to set output timeout %d, err %d", timeout, err);
		goto error;
	}

	err = mpp_init(mMppCtx, MPP_CTX_ENC, mCodingType);
	if (err != MPP_OK) {
		ALOGE("failed to mpp_init, err %d", err);
		goto error;
	}

	if (setupEncCfg() != C2_OK) {
		ALOGE("failed to set config");
		goto error;
	}

	mStarted = true;

	return C2_OK;

error:
	if (mMppCtx) {
		mpp_destroy(mMppCtx);
		mMppCtx = nullptr;
	}

	return C2_CORRUPTED;
}

c2_status_t C2RkMppEncoder::handleCommonDynamicCfg(void)
{
	bool change = false;

	IntfImpl::Lock lock = mIntf->lock();
	std::shared_ptr<C2StreamPictureSizeInfo::input> size = mIntf->getSize_l();
	std::shared_ptr<C2StreamBitrateInfo::output> bitrate = mIntf->getBitrate_l();
	std::shared_ptr<C2StreamFrameRateInfo::output> frameRate = mIntf->getFrameRate_l();
	lock.unlock();

	// handle dynamic size config.
	if (size != mSize) {
		ALOGI("new size request, w %d h %d", size->width, size->height);
		mSize = size;
		setupBaseCodec();
		change = true;
	}

	// handle dynamic bitrate config.
	if (bitrate != mBitrate) {
		ALOGI("new bitrate request, value %d", bitrate->value);
		mBitrate = bitrate;
		setupBitRate();
		change = true;
	}

	// handle dynamic frameRate config.
	if (frameRate != mFrameRate) {
		ALOGI("new frameRate request, value %.2f", frameRate->value);
		mFrameRate = frameRate;
		setupFrameRate();
		change = true;
	}

	if (change) {
		MPP_RET err = mMppMpi->control(mMppCtx, MPP_ENC_SET_CFG, mEncCfg);
		if (err != MPP_OK) {
			ALOGE("failed to setup dynamic config, ret %d", err);
		}
	}

	return C2_OK;
}

c2_status_t C2RkMppEncoder::handleRequestSyncFrame()
{
	int32_t layerPos = 0;

	// TODO Is there a better way to count frame layer?
	if (mCurLayerCount >= 2) {
		layerPos = mInputCount % (2 << (mCurLayerCount - 2));
	}

	// only handle IDR request at layer 0
	if (layerPos == 0) {
		IntfImpl::Lock lock = mIntf->lock();
		std::shared_ptr<C2StreamRequestSyncFrameTuning::output> requestSync;
		requestSync = mIntf->getRequestSync_l();
		lock.unlock();

		// we can handle IDR immediately
		if (requestSync->value) {
			ALOGI("got sync request");
			// unset request
			C2StreamRequestSyncFrameTuning::output clearSync(0u, C2_FALSE);
			std::vector<std::unique_ptr<C2SettingResult>> failures;
			mIntf->config({ &clearSync }, C2_MAY_BLOCK, &failures);
			// force set IDR frame
			mMppMpi->control(mMppCtx, MPP_ENC_SET_IDR_FRAME, nullptr);
		}
	}

	return C2_OK;
}

void C2RkMppEncoder::process(
	const std::unique_ptr<C2Work> &work,
	const std::shared_ptr<C2BlockPool> &pool)
{
	c2_status_t err = C2_OK;

	// Initialize output work
	work->result = C2_OK;
	work->workletsProcessed = 0u;
	work->worklets.front()->output.flags = work->input.flags;

	// Initialize encoder if not already initialized
	if (!mStarted) {
		err = initEncoder();
		if (err != C2_OK) {
			work->result = C2_BAD_VALUE;
			ALOGE("failed to initialize, signalled Error");
			return;
		}
	}

	if (mSignalledError) {
		ALOGI("Signalled Error");
		work->result = C2_BAD_VALUE;
		work->workletsProcessed = 1u;
		return;
	}

	std::shared_ptr<const C2GraphicView> view;
	std::shared_ptr<C2Buffer> inputBuffer = nullptr;
	if (!work->input.buffers.empty()) {
		inputBuffer = work->input.buffers[0];
		view = std::make_shared<const C2GraphicView>(
			inputBuffer->data().graphicBlocks().front().map().get());
		if (view->error() != C2_OK) {
			ALOGE("graphic view map err = %d", view->error());
			mSignalledError = true;
			work->result = C2_CORRUPTED;
			work->workletsProcessed = 1u;
			return;
		}
		const C2GraphicView *const input = view.get();
		if ((input != nullptr) && (input->width() < mSize->width ||
			input->height() < mSize->height)) {
			/* Expect width height to be configured */
			ALOGE("unexpected Capacity Aspect %d(%d) x %d(%d)",
				input->width(), mSize->width, input->height(), mSize->height);
			mSignalledError = true;
			work->result = C2_CORRUPTED;
			work->workletsProcessed = 1u;
			return;
		}
	}

	uint32_t flags = work->input.flags;
	uint64_t frameIndex = work->input.ordinal.frameIndex.peekull();
	uint64_t timestamp = work->input.ordinal.timestamp.peekll();

	ALOGV("process one work timestamp %lu frameindex %lu, flags %x",
		timestamp, frameIndex, flags);

	mSawInputEOS = (flags & C2FrameData::FLAG_END_OF_STREAM);

	if (!mSpsPpsHeaderReceived) {
		MppPacket hdrPkt   = nullptr;
		void     *hdrBuf   = nullptr;
		void     *data     = nullptr;
		uint32_t  hdrSize  = 1024;
		uint32_t  dataSize = 0;

		hdrBuf = malloc(hdrSize * sizeof(uint8_t));
		if (hdrBuf)
			mpp_packet_init(&hdrPkt, hdrBuf, hdrSize);

		if (hdrPkt) {
			mMppMpi->control(mMppCtx, MPP_ENC_GET_HDR_SYNC, hdrPkt);
			data = mpp_packet_get_data(hdrPkt);
			dataSize = mpp_packet_get_length(hdrPkt);
		}

		std::unique_ptr<C2StreamInitDataInfo::output> csd =
			C2StreamInitDataInfo::output::AllocUnique(dataSize, 0u);

		memcpy(csd->m.value, data, dataSize);
		work->worklets.front()->output.configUpdate.push_back(std::move(csd));

		mSpsPpsHeaderReceived = true;

		if (hdrPkt) {
			mpp_packet_deinit(&hdrPkt);
			hdrPkt = nullptr;
		}
		if (hdrBuf) {
			free(hdrBuf);
			hdrBuf = nullptr;
		}

		if (work->input.buffers.empty()) {
			work->workletsProcessed = 1u;
			return;
		}
	}

	// handle common dynamic config change
	handleCommonDynamicCfg();

	MyDmaBuffer_t inDmaBuf;
	OutWorkEntry entry;

	memset(&inDmaBuf, 0, sizeof(MyDmaBuffer_t));
	memset(&entry, 0, sizeof(OutWorkEntry));

	err = getInBufferFromWork(work, &inDmaBuf);
	if (err != C2_OK) {
		mSignalledError = true;
		work->result = C2_CORRUPTED;
		work->workletsProcessed = 1u;
		return;
	}

	/* send frame to mpp */
	err = sendFrame(inDmaBuf, frameIndex, flags);
	if (C2_OK != err) {
		ALOGE("failed to enqueue frame, err %d", err);
		mSignalledError = true;
		work->result = C2_CORRUPTED;
		work->workletsProcessed = 1u;
		return;
	}

	/* get packet from mpp */
	err = getOutPacket(&entry);
	if (err == C2_OK) {
		finishWork(work, pool, entry);
	} else {
		if (work && work->workletsProcessed != 1u) {
			fillEmptyWork(work);
		}
	}

	if (!mSawInputEOS && work->input.buffers.empty()) {
		fillEmptyWork(work);
	}
}

c2_status_t C2RkMppEncoder::drain(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool)
{
	(void)drainMode;
	(void)pool;
	return C2_OK;
}

// Note: Check if the input can be received by mpp driver directly
bool C2RkMppEncoder::isColorFmtConvertNeeded(uint32_t width, uint32_t height, MppFrameFormat fmt)
{
	bool needsRga = true;

	if (fmt == MPP_FMT_RGBA8888) {
		uint32_t vcodec_type = mpp_get_vcodec_type();
		if (vcodec_type & HAVE_RKVENC)
			needsRga = true;
	}

	// FIXME: This is only used for RK3588/RK3576/RK3562/RK3528
	needsRga = (mCodingType == MPP_VIDEO_CodingVP8);

	if (needsRga && C2_IS_ALIGNED(width, 16) && C2_IS_ALIGNED(height, 16)) {
		needsRga = false;
	}

cleanUp:
	if (mInputCount == 0) {
		ALOGI("check: hor %d ver %d fmt %s %s extra convert",
			width, height, toStr_Format(fmt), needsRga ? "need" : "no need");
	}
	return needsRga;
}

c2_status_t C2RkMppEncoder::getInBufferFromWork(
	const std::unique_ptr<C2Work> &work, MyDmaBuffer_t *outBuffer)
{
	c2_status_t ret = C2_OK;
	uint64_t frameIndex = work->input.ordinal.frameIndex.peekull();
	bool configChanged = false;

	if (work->input.buffers.empty()) {
		ALOGW("ignore empty input with frameIndex %ld", frameIndex);
		return C2_OK;
	}

	std::shared_ptr<const C2GraphicView> view;
	std::shared_ptr<C2Buffer> inputBuffer = nullptr;

	inputBuffer = work->input.buffers[0];
	view = std::make_shared<const C2GraphicView>(
		inputBuffer->data().graphicBlocks().front().map().get());
	const C2GraphicView* const input = view.get();
	const C2PlanarLayout& layout = input->layout();
	const C2Handle *c2Handle = inputBuffer->data().graphicBlocks().front().handle();

	uint32_t bqSlot, width, height, format, stride, generation;
	uint64_t usage, bqId;

	android::_UnwrapNativeCodec2GrallocMetadata(
		c2Handle, &width, &height, &format, &usage,
		&stride, &generation, &bqId, &bqSlot);

	// Fix error for wifidisplay when stride is 0
	if (stride == 0) {
		std::vector<ui::PlaneLayout> layouts;
		buffer_handle_t bufferHandle;
		native_handle_t *grallocHandle = UnwrapNativeCodec2GrallocHandle(c2Handle);

		GraphicBufferMapper &gm(GraphicBufferMapper::get());
		gm.importBuffer(const_cast<native_handle_t *>(grallocHandle),
				width, height, 1, format, usage,
				stride, &bufferHandle);
		gm.getPlaneLayouts(const_cast<native_handle_t *>(bufferHandle), &layouts);
		if (layouts[0].sampleIncrementInBits != 0) {
			stride = layouts[0].strideInBytes * 8 / layouts[0].sampleIncrementInBits;
		} else {
			ALOGE("layouts[0].sampleIncrementInBits = 0");
			stride = mHorStride;
		}
		gm.freeBuffer(bufferHandle);
		native_handle_delete(grallocHandle);
	}

	ALOGV("in buffer attr. w %d h %d stride %d layout 0x%x frameIndex %ld",
		width, height, stride, layout.type, frameIndex);

	switch (layout.type) {
	case C2PlanarLayout::TYPE_RGB:
		[[fallthrough]];
	case C2PlanarLayout::TYPE_RGBA: {
		uint32_t fd = c2Handle->data[0];

		if (!isColorFmtConvertNeeded(stride, height, MPP_FMT_RGBA8888)) {
			if (mHorStride != stride || mVerStride != height) {
				// setup encoder using new stride config
				ALOGI("cfg stride change from [%d:%d] -> [%d %d]",
					mHorStride, mVerStride, stride, height);
				mHorStride = stride;
				mVerStride = height;
				configChanged = true;
			}

			if (mInputMppFmt != MPP_FMT_RGBA8888) {
				ALOGI("update use rgba input format.");
				mInputMppFmt = MPP_FMT_RGBA8888;
				configChanged = true;
			}

			outBuffer->fd = fd;
			outBuffer->size = mHorStride * mVerStride * 4;
		} else {
			if (!csc_gles_init()) {
				ret = C2_CORRUPTED;
			}

			csc_gles_imported_buffer_t *buffer = nullptr;
			//csc_gles_import_mem(CSC_GLES_BUFFER_TYPE_RGBA8888, width, height, rgba_data, &buffer);
			csc_gles_import_dmabuf(CSC_GLES_BUFFER_TYPE_RGBA8888, width, height, fd, &buffer);
			GLuint ssbo;
			size_t nv12_data_size;
			csc_gles_convert_rgba8888_to_nv12(buffer, &ssbo, &nv12_data_size);

			void *mapped = nullptr;
			csc_gles_ssbo_map(ssbo, nv12_data_size, &mapped);
			if (!mapped) {
				ALOGE("Failed to fetch the converted NV12 data.");
				ret = C2_CORRUPTED;
			}
			if (nv12_data_size != mHorStride * mVerStride * 3 / 2) {
				ALOGE("Mismatched size of data! Expected %zu but received %zu.",
				(size_t) mHorStride * mVerStride * 3 / 2, nv12_data_size);
				ret = C2_CORRUPTED;
			}
			void *dma_buf = mmap(NULL, nv12_data_size,
				PROT_READ | PROT_WRITE,
				MAP_SHARED, fd, 0);
			if (!dma_buf) {
				ALOGE("Failed to access the temp DMA-BUF.");
				ret = C2_CORRUPTED;
			}
			memcpy(dma_buf, mapped, nv12_data_size);
		
			csc_gles_ssbo_unmap(ssbo);
		
			csc_gles_deinit();

			outBuffer->fd = mDmaMem->fd;
			outBuffer->size = nv12_data_size;
		}
	} break;
	case C2PlanarLayout::TYPE_YUV: {
		uint32_t fd = c2Handle->data[0];
		
		if (mInputMppFmt != MPP_FMT_YUV420SP) {
			ALOGI("update use yuv input format.");
			mInputMppFmt = MPP_FMT_YUV420SP;
			configChanged = true;
		}
	    
		if (!isColorFmtConvertNeeded(stride, height, MPP_FMT_YUV420SP)) {
			if (mHorStride != stride || mVerStride != height) {
				// setup encoder using new stride config
				ALOGI("cfg stride change from [%d:%d] -> [%d %d]",
					mHorStride, mVerStride, stride, height);
				mHorStride = stride;
				mVerStride = height;
				configChanged = true;
			}
			outBuffer->fd = fd;
			outBuffer->size = mHorStride * mVerStride * 3 / 2;
		} else {
			//RgaInfo src, dst;

			//C2RKRgaDef::SetRgaInfo(&src, fd, width, height, stride, height);
			//C2RKRgaDef::SetRgaInfo(&dst, mDmaMem->fd,
			//	mSize->width, mSize->height, mHorStride, mVerStride);
			//if (!C2RKRgaDef::NV12ToNV12(src, dst)) {
			//	ret = C2_CORRUPTED;
			//}
		    
			outBuffer->fd = mDmaMem->fd;
			outBuffer->size = mHorStride * mVerStride * 3 / 2;
		}
	} break;
	default:
		ALOGE("Unrecognized plane type: %d", layout.type);
		ret = C2_BAD_VALUE;
	}

	if (configChanged) {
		if (mInputMppFmt == MPP_FMT_RGBA8888) {
			mpp_enc_cfg_set_s32(mEncCfg, "prep:hor_stride", mHorStride * 4);
		} else {
			mpp_enc_cfg_set_s32(mEncCfg, "prep:hor_stride", mHorStride);
		}
		mpp_enc_cfg_set_s32(mEncCfg, "prep:ver_stride", mVerStride);
		mpp_enc_cfg_set_s32(mEncCfg, "prep:format", mInputMppFmt);
	    
		MPP_RET err = mMppMpi->control(mMppCtx, MPP_ENC_SET_CFG, mEncCfg);
		if (err != MPP_OK) {
			ALOGE("failed to setup new mpp config.");
			ret = C2_CORRUPTED;
		}
	}

	return ret;
}

c2_status_t C2RkMppEncoder::sendFrame(
	MyDmaBuffer_t dBuffer, uint64_t pts, uint32_t flags)
{
	c2_status_t ret = C2_OK;
	MPP_RET err = MPP_OK;
	MppFrame frame = nullptr;

	mpp_frame_init(&frame);

	if (flags & C2FrameData::FLAG_END_OF_STREAM) {
		ALOGI("send input eos");
		mpp_frame_set_eos(frame, 1);
	}

	ALOGV("send frame fd %d size %ld pts %ld", dBuffer.fd, dBuffer.size, pts);

	if (dBuffer.fd > 0) {
		MppBuffer buffer = nullptr;
		MppBufferInfo commit;
		
		memset(&commit, 0, sizeof(commit));
		
		commit.type = MPP_BUFFER_TYPE_ION;
		commit.fd = dBuffer.fd;
		commit.size = dBuffer.size;
		
		err = mpp_buffer_import(&buffer, &commit);
		if (err) {
			ALOGE("failed to import input buffer");
			ret = C2_NOT_FOUND;
			goto error;
		}
		mpp_frame_set_buffer(frame, buffer);
		mpp_buffer_put(buffer);
		buffer = nullptr;
	} else {
		mpp_frame_set_buffer(frame, nullptr);
	}

	mpp_frame_set_width(frame, mSize->width);
	mpp_frame_set_height(frame, mSize->height);
	mpp_frame_set_ver_stride(frame, mVerStride);
	mpp_frame_set_pts(frame, pts);
	mpp_frame_set_fmt(frame, mInputMppFmt);

	switch(mInputMppFmt) {
	case MPP_FMT_RGBA8888:
		mpp_frame_set_hor_stride(frame, mHorStride * 4);
		break;
	case MPP_FMT_YUV420P:
	case MPP_FMT_YUV420SP:
		mpp_frame_set_hor_stride(frame, mHorStride);
		break;
	default:
		break;
	}

	/* handle IDR request */
	handleRequestSyncFrame();

	err = mMppMpi->encode_put_frame(mMppCtx, frame);
	if (err != MPP_OK) {
		ALOGE("failed to put_frame, err %d", err);
		ret = C2_NOT_FOUND;
		goto error;
	}

	mInputCount++;

error:
	if (frame) {
		mpp_frame_deinit(&frame);
	}

	return ret;
}

c2_status_t C2RkMppEncoder::getOutPacket(OutWorkEntry *entry)
{
	MPP_RET err = MPP_OK;
	MppPacket packet = nullptr;

	err = mMppMpi->encode_get_packet(mMppCtx, &packet);
	if (err != MPP_OK) {
		return C2_NOT_FOUND;
	} else {
		int64_t  pts = mpp_packet_get_pts(packet);
		size_t   len = mpp_packet_get_length(packet);
		uint32_t eos = mpp_packet_get_eos(packet);
		void   *data = mpp_packet_get_data(packet);

		ALOGV("get outpacket pts %ld size %ld eos %d", pts, len, eos);

		if (eos) {
			ALOGI("get output eos");
			mOutputEOS = true;
			if (pts == 0 || !len) {
				ALOGI("eos with empty pkt");
				return C2_CORRUPTED;
			}
		}

		if (!len) {
			ALOGW("ignore empty output with pts %ld", pts);
			return C2_CORRUPTED;
		}

		entry->frameIndex = pts;
		entry->outPacket  = packet;

		return C2_OK;
	}
}

void C2RkMppEncoder::fillEmptyWork(const std::unique_ptr<C2Work>& work)
{
	uint32_t flags = 0;

	ALOGV("called fillEmptyWork");

	if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
		flags |= C2FrameData::FLAG_END_OF_STREAM;
		ALOGI("Signalling EOS");
	}
	work->worklets.front()->output.flags = (C2FrameData::flags_t)flags;
	work->worklets.front()->output.buffers.clear();
	work->worklets.front()->output.ordinal = work->input.ordinal;
	work->workletsProcessed = 1u;
}

void C2RkMppEncoder::finishWork(
	const std::unique_ptr<C2Work> &work,
	const std::shared_ptr<C2BlockPool>& pool,
	OutWorkEntry entry)
{
	c2_status_t ret = C2_OK;
	uint64_t frmIndex = 0;
	MppPacket packet = nullptr;
	std::shared_ptr<C2LinearBlock> block;
	C2MemoryUsage usage = { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE };

	frmIndex = entry.frameIndex;
	packet = entry.outPacket;

	void   *data = mpp_packet_get_data(packet);
	size_t  len  = mpp_packet_get_length(packet);
	size_t  size = mpp_packet_get_size(packet);

	ret = pool->fetchLinearBlock(size, usage, &block);
	if (ret != C2_OK) {
		ALOGE("failed to fetch block for output, ret 0x%x", ret);
		work->result = ret;
		work->workletsProcessed = 1u;
		mSignalledError = true;
		return;
	}

	C2WriteView wView = block->map().get();
	if (C2_OK != wView.error()) {
		ALOGE("write view map failed with status 0x%x", wView.error());
		work->result = wView.error();
		work->workletsProcessed = 1u;
		mSignalledError = true;
		return;
	}

	// copy mpp output to c2 output
	memcpy(wView.data(), data, len);

	RK_S32 isIntra = 0;
	std::shared_ptr<C2Buffer> buffer = createLinearBuffer(block, 0, len);
	MppMeta meta = mpp_packet_get_meta(packet);
	mpp_meta_get_s32(meta, KEY_OUTPUT_INTRA, &isIntra);
	if (isIntra) {
		ALOGI("IDR frame produced");
		buffer->setInfo(std::make_shared<C2StreamPictureTypeMaskInfo::output>(
			0u /* stream id */, C2Config::SYNC_FRAME));
	}

	mpp_packet_deinit(&packet);

	auto fillWork = [buffer](const std::unique_ptr<C2Work> &work) {
		work->worklets.front()->output.flags = (C2FrameData::flags_t)0;
		work->worklets.front()->output.buffers.clear();
		work->worklets.front()->output.buffers.push_back(buffer);
		work->worklets.front()->output.ordinal = work->input.ordinal;
		work->workletsProcessed = 1u;
	};

	if (work && c2_cntr64_t(frmIndex) == work->input.ordinal.frameIndex) {
		fillWork(work);
		if (mSawInputEOS) {
		    work->worklets.front()->output.flags = C2FrameData::FLAG_END_OF_STREAM;
		}
	} else {
		finish(frmIndex, fillWork);
	}
}

uint32_t getMppBitrateMode(int32_t mode, bool c2Type)
{
	struct BitrateModeMap {
		uint32_t c2Mode;
		uint32_t codecMode;
		uint32_t mppMode;
	};

	static const BitrateModeMap kBModeMaps[] = {
		{ C2Config::BITRATE_IGNORE,		BITRATE_MODE_CQ,	MPP_ENC_RC_MODE_FIXQP },
		{ C2Config::BITRATE_VARIABLE,		BITRATE_MODE_VBR,	MPP_ENC_RC_MODE_VBR },
		{ C2Config::BITRATE_CONST,		BITRATE_MODE_CBR,	MPP_ENC_RC_MODE_CBR },
		{ C2Config::BITRATE_CONST_SKIP_ALLOWED,	BITRATE_MODE_CBR_FD,	MPP_ENC_RC_MODE_CBR },
	};

	static const size_t kNumBModeMaps = sizeof(kBModeMaps) / sizeof(kBModeMaps[0]);

	int32_t i;
	for (i = 0; i < kNumBModeMaps; i++) {
		uint32_t dstMode = c2Type ? kBModeMaps[i].c2Mode : kBModeMaps[i].codecMode;
		if (dstMode == mode)
			break;
	}

	if (i == kNumBModeMaps) {
		ALOGW("get unsupport %s bitrate mode %d, set default cbr mode",
			c2Type ? "c2" : "codec", mode);
		return MPP_ENC_RC_MODE_CBR;
	}

	return kBModeMaps[i].mppMode;
}

} // namespace codec2::rkmpp
