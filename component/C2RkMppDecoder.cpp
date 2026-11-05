//#define LOG_NDEBUG 0
#define LOG_TAG "C2RkMppDecoder"
#include <log/log.h>

// libmpp requires this.
#undef MODULE_TAG
#define MODULE_TAG "C2RkMppDecoder"

#include <inttypes.h>
#include <string.h>
#include <unistd.h>

#include <memory>
#include <vector>

#include <media/stagefright/foundation/ALookup.h>
#include <media/stagefright/MediaCodecConstants.h>

#include <Codec2Mapper.h>
#include <C2AllocatorGralloc.h>
#include <C2RkMppDecoder.hpp>
#include <ui/GraphicBufferMapper.h>

#include <mpp/mpp_buffer.h>
#include <mpp/mpp_frame.h>
#include <mpp/mpp_packet.h>
#include <mpp/rk_mpi.h>

#include <vendor_rockchip.h>

namespace codec2::rkmpp {

c2_status_t importGraphicBuffer(const C2Handle *const c2Handle, buffer_handle_t *outHandle)
{
	uint32_t bqSlot, width, height, format, stride, generation;
	uint64_t usage, bqId;

	native_handle_t *gHandle = UnwrapNativeCodec2GrallocHandle(c2Handle);

	android::_UnwrapNativeCodec2GrallocMetadata(
		c2Handle, &width, &height, &format, &usage,
		&stride, &generation, &bqId, &bqSlot);

	status_t err = GraphicBufferMapper::get().importBuffer(
		gHandle, width, height, 1, format, usage,
		stride, outHandle);
	if (err != OK) {
		ALOGE("failed to import buffer %p", gHandle);
	}

	native_handle_delete(gHandle);
	return (c2_status_t)err;
}

void freeGraphicBuffer(buffer_handle_t outHandle)
{
	if (outHandle) {
		GraphicBufferMapper::get().freeBuffer(outHandle);
	}
}

static int32_t frameReadyCb(void *ctx, void *mppCtx, int32_t cmd, void *frame)
{
	(void)mppCtx;
	(void)cmd;
	(void)frame;
	C2RkMppDecoder *decoder = (C2RkMppDecoder *) ctx;
	decoder->postFrameReady();
	return 0;
}

void C2RkMppDecoder::WorkHandler::waitAllMsgFlushed()
{
	sp<AMessage> msg = new AMessage(WorkHandler::kWhatFlushMessage, this);

	sp<AMessage> response;
	msg->postAndAwaitResponse(&response);
}

void C2RkMppDecoder::WorkHandler::onMessageReceived(const sp<AMessage> &msg)
{
	switch (msg->what()) {
		case kWhatFrameReady: {
			C2RkMppDecoder *thiz = nullptr;
			if (msg->findPointer("thiz", (void **)(&thiz)) && thiz) {
				thiz->onFrameReady();
			}
		} break;
		case kWhatFlushMessage: {
			sp<AReplyToken> replyID;
			msg->senderAwaitsResponse(&replyID);

			sp<AMessage> response = new AMessage;
			response->postReply(replyID);
		} break;
		default: {
			ALOGE("Unrecognized msg: %d", msg->what());
		} break;
	}
}

C2RkMppDecoder::C2RkMppDecoder(
	const char *name,
	const char *mime,
	c2_node_id_t id,
	const std::shared_ptr<IntfImpl> &intfImpl)
	: C2RkMppComponent(std::make_shared<C2RkMppComponentInterface<IntfImpl>>(name, id, intfImpl)),
	mIntf(intfImpl),
	mLooper(nullptr),
	mHandler(nullptr),
	mMppCtx(nullptr),
	mMppMpi(nullptr),
	mCodingType(MPP_VIDEO_CodingUnused),
	mColorFormat(kMppFrameFormat),
	mFrmGrp(nullptr),
	mWidth(0),
	mHeight(0),
	mHorStride(0),
	mVerStride(0),
	mStarted(false),
	mFlushed(true),
	mSignalledInputEos(false),
	mOutputEos(false),
	mSignalledError(false),
	mLowLatencyMode(false),
	mFlushing(false),
	mPendingConfigUpdate(false)
{
	mCodingType = (MppCodingType) mppCodingFromMime(mime);
	if (mCodingType == MPP_VIDEO_CodingUnused) {
		ALOGE("failed to get coding from name %s", name);
	}
}

C2RkMppDecoder::~C2RkMppDecoder()
{
	onRelease();
}

c2_status_t C2RkMppDecoder::onInit()
{
	c2_status_t err = updateOutputDelay();
	if (err != C2_OK) {
		ALOGE("failed to update output delay");
	}

	err = setupAndStartLooper();
	if (err != C2_OK) {
		ALOGE("failed to start looper therad");
	}

	return err;
}

c2_status_t C2RkMppDecoder::onStop()
{
	if (!mFlushed) {
		return onFlush_sm();
	}

	return C2_OK;
}

void C2RkMppDecoder::onReset()
{
	onStop();
}

void C2RkMppDecoder::onRelease()
{
	if (!mFlushed) {
		onFlush_sm();
	}

	{
		Mutex::Autolock autoLock(mBufferLock);
		clearOutBuffers();
	}

	mBlockPool.reset();

	if (mFrmGrp != nullptr) {
		mpp_buffer_group_put(mFrmGrp);
		mFrmGrp = nullptr;
	}

	if (mMppCtx) {
		mpp_destroy(mMppCtx);
		mMppCtx = nullptr;
	}

	stopAndReleaseLooper();

	mStarted = false;
}

c2_status_t C2RkMppDecoder::onFlush_sm()
{
	/*
	 * 先标记 flushing，并等待解码线程上已经排队的 frame-ready 消息处理完，
	 * 再 reset MPP，避免复位时还有回调在使用 MPP 的 buffer。
	 *
	 * 这里故意不做 mFlushed 提前返回：即使还没成功送过包，也可能已经
	 * 导入过 buffer 或者置上了 mSignalledError，flush 必须把这些状态清掉。
	 */
	mFlushing = true;

	if (mHandler) {
		mHandler->waitAllMsgFlushed();
	}

	{
		Mutex::Autolock autoLock(mBufferLock);

		/*
		 * reset 必须排在清理 buffer group 之前：MPP 内部（DPB）可能还
		 * 持有 group 里的 buffer，先复位让它释放这些引用。
		 */
		if (mMppMpi) {
			mMppMpi->reset(mMppCtx);
		}

		clearOutBuffers();
		if (mFrmGrp) {
			mpp_buffer_group_clear(mFrmGrp);
		}
	}

	{
		Mutex::Autolock autoLock(mPendingLock);
		mPtsToFrameIndex.clear();
	}

	{
		Mutex::Autolock autoLock(mEosLock);
		mSignalledInputEos = false;
		mOutputEos = false;
	}

	mSignalledError = false;
	mPendingConfigUpdate = false;
	mFlushed = true;
	mFlushing = false;

	return C2_OK;
}

c2_status_t C2RkMppDecoder::setupAndStartLooper()
{
	status_t err = OK;

	if (mLooper == nullptr) {
		mLooper = new ALooper;
		mHandler = new WorkHandler;

		mLooper->setName("C2DecLooper");
		err = mLooper->start();
		if (err == OK) {
			mLooper->registerHandler(mHandler);
		}
	}
	return (c2_status_t)err;
}

void C2RkMppDecoder::stopAndReleaseLooper()
{
	if (mLooper != nullptr) {
		if (mHandler != nullptr) {
			mLooper->unregisterHandler(mHandler->id());
			mHandler.clear();
		}
		mLooper->stop();
		mLooper.clear();
	}
}

/*
 * 参考帧数量决定需要向 MPP 外部 buffer group 预导入多少块输出 buffer：
 * 太少会让 MPP 等待参考帧时停住，太多则浪费内存。
 */
static uint32_t calculateVideoRefCount(MppCodingType coding)
{
	switch (coding) {
		case MPP_VIDEO_CodingAVC:
		case MPP_VIDEO_CodingHEVC:
		case MPP_VIDEO_CodingAV1:
			return 16;
		case MPP_VIDEO_CodingVP9:
			return 8;
		default:
			return C2_DEFAULT_REF_FRAME_COUNT;
	}
}

c2_status_t C2RkMppDecoder::updateOutputDelay()
{
	c2_status_t err = C2_OK;
	uint32_t width = 0, height = 0;

	{
		IntfImpl::Lock lock = mIntf->lock();
		width  = mIntf->getSize_l()->width;
		height = mIntf->getSize_l()->height;
	}

	uint32_t refCnt = calculateVideoRefCount(mCodingType);

	ALOGI("Codec(%s %dx%d) needs %u reference frames",
		toStr_Coding(mCodingType), width, height, refCnt);

	C2PortActualDelayTuning::output delayTuning(refCnt);
	std::vector<std::unique_ptr<C2SettingResult>> failures;

	err = mIntf->config({&delayTuning}, C2_MAY_BLOCK, &failures);
	if (err != C2_OK) {
		ALOGE("failed to config delay tuning, err %d", err);
	}

	return err;
}

c2_status_t C2RkMppDecoder::initDecoder()
{
	MPP_RET err = MPP_OK;

	{
		IntfImpl::Lock lock = mIntf->lock();
		mWidth = mIntf->getSize_l()->width;
		mHeight = mIntf->getSize_l()->height;
		mLowLatencyMode = mIntf->getIsLowLatencyMode();
		mColorFormat = kMppFrameFormat;
	}

	ALOGI("init: w %d h %d coding %s", mWidth, mHeight, toStr_Coding(mCodingType));

	err = mpp_create(&mMppCtx, &mMppMpi);
	if (err != MPP_OK) {
		ALOGE("failed to mpp_create, ret %d", err);
		goto error;
	}

	// TODO: workround: CTS-CodecDecoderTest
	// testFlushNative[15(c2.rk.mpeg2.decoder_video/mpeg2)
	if (mCodingType == MPP_VIDEO_CodingMPEG2) {
		uint32_t mode = 0, split = 1;
		mMppMpi->control(mMppCtx, MPP_DEC_SET_ENABLE_DEINTERLACE, &mode);
		mMppMpi->control(mMppCtx, MPP_DEC_SET_PARSER_SPLIT_MODE, &split);
	} else {
		// enable deinterlace, but not decting
		uint32_t mode = 1;
		mMppMpi->control(mMppCtx, MPP_DEC_SET_ENABLE_DEINTERLACE, &mode);
	}

	{
		uint32_t fastParse = 1;
		mMppMpi->control(mMppCtx, MPP_DEC_SET_PARSER_FAST_MODE, &fastParse);

		uint32_t fastPlay = 1;
		mMppMpi->control(mMppCtx, MPP_DEC_SET_ENABLE_FAST_PLAY, &fastPlay);
		
		if (mLowLatencyMode) {
			uint32_t fastOut = 1;
			mMppMpi->control(mMppCtx, MPP_DEC_SET_IMMEDIATE_OUT, &fastOut);
			ALOGI("enable lowLatency, enable mpp fast-out mode");
		}

		IntfImpl::Lock lock = mIntf->lock();
		//if (mIntf->getDisableDpbCheck_l() && mIntf->getDisableDpbCheck_l()->value > 0) {
		//	uint32_t disableCheck = 1;
		//	mMppMpi->control(mMppCtx, MPP_DEC_SET_DISABLE_DPB_CHECK, &disableCheck);
		//	ALOGI("disable poc discontinuous check");
		//}
	}

	err = mpp_init(mMppCtx, MPP_CTX_DEC, mCodingType);
	if (err != MPP_OK) {
		ALOGE("failed to mpp_init, err %d", err);
		goto error;
	}

	{
		MppFrame frame  = nullptr;

		uint32_t mppFmt = mColorFormat;

		mMppMpi->control(mMppCtx, MPP_DEC_SET_OUTPUT_FORMAT, (MppParam)&mppFmt);

		mpp_frame_init(&frame);
		mpp_frame_set_width(frame, mWidth);
		mpp_frame_set_height(frame, mHeight);
		mpp_frame_set_fmt(frame, (MppFrameFormat)mppFmt);

		err = mMppMpi->control(mMppCtx, MPP_DEC_SET_FRAME_INFO, (MppParam)frame);
		if (err != MPP_OK) {
			ALOGE("failed to set frame info, err %d", err);
			mpp_frame_deinit(&frame);
			goto error;
		}

		mHorStride   = mpp_frame_get_hor_stride(frame);
		mVerStride   = mpp_frame_get_ver_stride(frame);
		mColorFormat = mpp_frame_get_fmt(frame);

		mpp_frame_deinit(&frame);

		ALOGI("init: hor %d ver %d color 0x%08x", mHorStride, mVerStride, mColorFormat);
	}

	err = mpp_buffer_group_get_external(&mFrmGrp, MPP_BUFFER_TYPE_ION);
	if (err != MPP_OK) {
		ALOGE("failed to get buffer_group, err %d", err);
		goto error;
	}

	err = mMppMpi->control(mMppCtx, MPP_DEC_SET_EXT_BUF_GROUP, mFrmGrp);
	if (err != MPP_OK) {
		ALOGE("failed to set buffer group, err %d", err);
		goto error;
	}

	{
		/* set output frame callback  */
		MppDecCfg cfg;
		mpp_dec_cfg_init(&cfg);
		mpp_dec_cfg_set_ptr(cfg, "cb:frm_rdy_cb", (void *)frameReadyCb);
		mpp_dec_cfg_set_ptr(cfg, "cb:frm_rdy_ctx", this);

		err = mMppMpi->control(mMppCtx, MPP_DEC_SET_CFG, cfg);
		if (err != MPP_OK) {
			ALOGE("failed to set frame callback, err %d", err);
			mpp_dec_cfg_deinit(cfg);
			goto error;
		}

		mpp_dec_cfg_deinit(cfg);
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

void C2RkMppDecoder::finishWork(OutWorkEntry entry)
{
	std::shared_ptr<C2Buffer> c2Buffer = nullptr;
	uint64_t timestamp = entry.timestamp;
	uint32_t flags = entry.flags;

	if (entry.outblock) {
		c2Buffer = createGraphicBuffer(
			std::move(entry.outblock), C2Rect(mWidth, mHeight));

		if (mCodingType == MPP_VIDEO_CodingAVC ||
			mCodingType == MPP_VIDEO_CodingHEVC ||
			mCodingType == MPP_VIDEO_CodingAV1 ||
			mCodingType == MPP_VIDEO_CodingMPEG2) {
			IntfImpl::Lock lock = mIntf->lock();
			c2Buffer->setInfo(mIntf->getColorAspects_l());
		}
	}

	if (flags & BUFFER_FLAGS_INFO_CHANGE) {
		ALOGI("update new size %dx%d config to framework.", mWidth, mHeight);
		mPendingConfigUpdate = true;
	}

	if (flags & BUFFER_FLAGS_EOS) {
		ALOGI("signalling eos");
		Mutex::Autolock autoLock(mEosLock);
		mOutputEos = true;
		mEosCondition.signal();
	}

	/*
	 * info-change 和空 EOS 帧只是解码器通知，不对应任何输入 work，
	 * 不能拿它们的（通常无效的）pts 去匹配 pending work。
	 */
	if ((flags & BUFFER_FLAGS_INFO_CHANGE) ||
		((flags & BUFFER_FLAGS_EOS) && !c2Buffer)) {
		return;
	}

	uint64_t frameIndex = 0;
	{
		Mutex::Autolock autoLock(mPendingLock);
		auto it = mPtsToFrameIndex.find(timestamp);
		if (it != mPtsToFrameIndex.end()) {
			frameIndex = it->second;
			mPtsToFrameIndex.erase(it);
		} else {
			/*
			 * 对不上 pts 说明上游已经 flush 掉对应 work，只能丢弃。
			 */
			if (c2Buffer) {
				ALOGE("drop output frame, unknown pts %" PRIu64, timestamp);
			}
			return;
		}
	}

	uint32_t outFlags = 0;
	if (flags & BUFFER_FLAGS_ERROR_FRAME) {
		outFlags |= C2FrameData::FLAG_DROP_FRAME;
	}

	bool configUpdate = mPendingConfigUpdate;
	mPendingConfigUpdate = false;
	uint32_t width = mWidth;
	uint32_t height = mHeight;
	uint32_t delayValue = mIntf->mActualOutputDelay->value;

	auto fillWork = [c2Buffer, outFlags, configUpdate, width, height, delayValue]
		(const std::unique_ptr<C2Work> &work) {
		uint32_t flags = outFlags;
		if (work->input.flags & C2FrameData::FLAG_DROP_FRAME) {
			flags |= C2FrameData::FLAG_DROP_FRAME;
		}

		work->worklets.front()->output.flags = (C2FrameData::flags_t)flags;
		work->worklets.front()->output.ordinal = work->input.ordinal;
		work->worklets.front()->output.buffers.clear();
		if (c2Buffer) {
			work->worklets.front()->output.buffers.push_back(c2Buffer);
		}
		if (configUpdate) {
			C2StreamPictureSizeInfo::output size(0u, width, height);
			C2PortActualDelayTuning::output delay(delayValue);
			work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(size));
			work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(delay));
		}
		work->workletsProcessed = 1u;
		work->result = C2_OK;
	};

	finish(frameIndex, fillWork);
}

c2_status_t C2RkMppDecoder::drain(
	uint32_t drainMode,
	const std::shared_ptr<C2BlockPool> &pool)
{
	(void)pool;

	if (drainMode == NO_DRAIN)
		return C2_OK;

	if (drainMode == DRAIN_CHAIN) {
		ALOGW("DRAIN_CHAIN not supported");
		return C2_OMITTED;
	}

	if (!mStarted || mFlushed || mSignalledInputEos || mSignalledError)
		return C2_OK;

	ALOGI("drain, send input eos");
	mSignalledInputEos = true;

	c2_status_t err = sendpacket(nullptr, 0, 0, C2FrameData::FLAG_END_OF_STREAM);
	if (err != C2_OK) {
		ALOGE("failed to send eos packet, err %d", err);
		mSignalledError = true;
		return err;
	}

	Mutex::Autolock autoLock(mEosLock);
	if (!mOutputEos)
		mEosCondition.wait(mEosLock);

	return C2_OK;
}

void C2RkMppDecoder::process(
	const std::unique_ptr<C2Work> &work,
	const std::shared_ptr<C2BlockPool> &pool)
{
	c2_status_t err = C2_OK;

	/*
	 * 输出帧由 frame-ready 回调异步返回，因此普通帧的 work 先保持
	 * workletsProcessed == 0，交给基类挂到 pending 队列，等输出帧到达时
	 * 再通过 finish(frameIndex, fillWork) 完成。
	 */
	work->result = C2_OK;
	work->workletsProcessed = 0u;

	// Initialize decoder if not already initialized
	if (!mStarted) {
		mBlockPool = pool;
		err = initDecoder();
		if (err != C2_OK) {
			work->result = C2_BAD_VALUE;
			work->workletsProcessed = 1u;
			ALOGE("failed to initialize, signalled Error");
			return;
		}
	}

	if (mSignalledInputEos || mSignalledError) {
		work->workletsProcessed = 1u;
		work->result = C2_CORRUPTED;
		return;
	}

	uint8_t *inData = nullptr;
	size_t inSize = 0u;
	C2ReadView rView = mDummyReadView;
	if (!work->input.buffers.empty()) {
		rView = work->input.buffers[0]->data().linearBlocks().front().map().get();
		inData = const_cast<uint8_t *>(rView.data());
		inSize = rView.capacity();
		if (inSize && rView.error()) {
			ALOGE("failed to read rView, error %d", rView.error());
			work->result = rView.error();
			work->workletsProcessed = 1u;
			return;
		}
	}

	uint32_t flags = work->input.flags;
	uint64_t frameIndex = work->input.ordinal.frameIndex.peekull();
	uint64_t timestamp = work->input.ordinal.timestamp.peekull();
	bool eos = (flags & C2FrameData::FLAG_END_OF_STREAM) != 0;
	bool config = (flags & C2FrameData::FLAG_CODEC_CONFIG) != 0;

	ALOGV("in buffer attr. size %zu timestamp %" PRIu64 " frameindex %" PRIu64 ", flags %x",
		inSize, timestamp, frameIndex, flags);

	/*
	 * 先登记 pts -> frameIndex，再送包：输出帧的回调里靠它找到对应 work。
	 * CSD 和 EOS 的 work 不产生自己的输出帧，不登记。
	 */
	if (!config && !eos) {
		Mutex::Autolock autoLock(mPendingLock);
		mPtsToFrameIndex[timestamp] = frameIndex;
	}

	err = ensureDecoderState();
	if (err != C2_OK) {
		if (!config && !eos) {
			Mutex::Autolock autoLock(mPendingLock);
			mPtsToFrameIndex.erase(timestamp);
		}
		mSignalledError = true;
		work->workletsProcessed = 1u;
		work->result = C2_CORRUPTED;
		return;
	}

	err = sendpacket(inData, inSize, timestamp, flags);
	if (err != C2_OK) {
		if (!config && !eos) {
			Mutex::Autolock autoLock(mPendingLock);
			mPtsToFrameIndex.erase(timestamp);
		}
		ALOGE("failed to send packet, pts %" PRIu64, timestamp);
		mSignalledError = true;
		work->workletsProcessed = 1u;
		work->result = C2_CORRUPTED;
		return;
	}

	mFlushed = false;

	if (eos) {
		mSignalledInputEos = true;
		// wait output eos stream
		Mutex::Autolock autoLock(mEosLock);
		if (!mOutputEos)
			mEosCondition.wait(mEosLock);

		work->worklets.front()->output.flags = C2FrameData::FLAG_END_OF_STREAM;
		work->worklets.front()->output.buffers.clear();
		work->worklets.front()->output.ordinal = work->input.ordinal;
		work->workletsProcessed = 1u;
	} else if (config) {
		// CSD/config work: MPP 不会为它输出帧，直接返回空输出。
		work->worklets.front()->output.flags = (C2FrameData::flags_t)0;
		work->worklets.front()->output.buffers.clear();
		work->worklets.front()->output.ordinal = work->input.ordinal;
		work->workletsProcessed = 1u;
	}
}

void C2RkMppDecoder::setDefaultCodecColorAspectsIfNeeded(android::ColorAspects &aspects)
{
	typedef android::ColorAspects CA;
	CA::MatrixCoeffs matrix;

	static const ALookup<CA::Primaries, CA::MatrixCoeffs> sPMAspectMap = {
		{
			{ CA::PrimariesUnspecified,   CA::MatrixUnspecified },
			{ CA::PrimariesBT709_5,       CA::MatrixBT709_5 },
			{ CA::PrimariesBT601_6_625,   CA::MatrixBT601_6 },
			{ CA::PrimariesBT601_6_525,   CA::MatrixBT601_6 },
			{ CA::PrimariesBT2020,        CA::MatrixBT2020 },
			{ CA::PrimariesBT470_6M,      CA::MatrixBT470_6M },
		}
	};

	// dataspace supported lists: BT709 / BT601_6_625 / BT601_6_525 / BT2020.
	// so reset unsupport aspect here. For unassigned aspect, reassignment will
	// do later in frameworks.
	if (aspects.mMatrixCoeffs == CA::MatrixOther)
		aspects.mMatrixCoeffs = CA::MatrixUnspecified;
	if (!sPMAspectMap.map(aspects.mPrimaries, &matrix)) {
		ALOGW("reset unsupport primaries %s", asString(aspects.mPrimaries));
		aspects.mPrimaries = CA::PrimariesUnspecified;
	}

	if (aspects.mMatrixCoeffs == CA::MatrixUnspecified && aspects.mPrimaries != CA::PrimariesUnspecified) {
		sPMAspectMap.map(aspects.mPrimaries, &aspects.mMatrixCoeffs);
	} else if (aspects.mPrimaries == CA::PrimariesUnspecified && aspects.mMatrixCoeffs != CA::MatrixUnspecified) {
		if (aspects.mMatrixCoeffs == CA::MatrixBT601_6) {
			if ((mWidth <= 720 && mHeight <= 480) || (mHeight <= 720 && mWidth <= 480)) {
				aspects.mPrimaries = CA::PrimariesBT601_6_525;
			} else {
				aspects.mPrimaries = CA::PrimariesBT601_6_625;
			}
		} else {
			if (!sPMAspectMap.map(aspects.mMatrixCoeffs, &aspects.mPrimaries)) {
				aspects.mMatrixCoeffs = CA::MatrixUnspecified;
			}
		}
	}
}

void C2RkMppDecoder::getVuiParams(MppFrame frame)
{
	VuiColorAspects aspects;

	aspects.primaries = mpp_frame_get_color_primaries(frame);
	aspects.transfer  = mpp_frame_get_color_trc(frame);
	aspects.coeffs    = mpp_frame_get_colorspace(frame);
	if (mCodingType == MPP_VIDEO_CodingMPEG2) {
		aspects.fullRange = 0;
	} else {
		aspects.fullRange = (mpp_frame_get_color_range(frame) == MPP_FRAME_RANGE_JPEG);
	}

	// convert vui aspects to C2 values if changed
	if (!(aspects == mBitstreamColorAspects)) {
		mBitstreamColorAspects = aspects;
		android::ColorAspects sfAspects;
		C2StreamColorAspectsInfo::input codedAspects = { 0u };
		
		ALOGI("Got vui color aspects, P(%d) T(%d) M(%d) R(%d)",
			aspects.primaries, aspects.transfer,
			aspects.coeffs, aspects.fullRange);

		ColorUtils::convertIsoColorAspectsToCodecAspects(
			aspects.primaries, aspects.transfer, aspects.coeffs,
			aspects.fullRange, sfAspects);

		setDefaultCodecColorAspectsIfNeeded(sfAspects);

		if (!C2Mapper::map(sfAspects.mPrimaries, &codedAspects.primaries)) {
			codedAspects.primaries = C2Color::PRIMARIES_UNSPECIFIED;
		}
		if (!C2Mapper::map(sfAspects.mRange, &codedAspects.range)) {
			codedAspects.range = C2Color::RANGE_UNSPECIFIED;
		}
		if (!C2Mapper::map(sfAspects.mMatrixCoeffs, &codedAspects.matrix)) {
			codedAspects.matrix = C2Color::MATRIX_UNSPECIFIED;
		}
		if (!C2Mapper::map(sfAspects.mTransfer, &codedAspects.transfer)) {
			codedAspects.transfer = C2Color::TRANSFER_UNSPECIFIED;
		}

		std::vector<std::unique_ptr<C2SettingResult>> failures;
		mIntf->config({&codedAspects}, C2_MAY_BLOCK, &failures);

		ALOGI("set colorAspects (R:%d(%s), P:%d(%s), M:%d(%s), T:%d(%s))",
			sfAspects.mRange, asString(sfAspects.mRange),
			sfAspects.mPrimaries, asString(sfAspects.mPrimaries),
			sfAspects.mMatrixCoeffs, asString(sfAspects.mMatrixCoeffs),
			sfAspects.mTransfer, asString(sfAspects.mTransfer));
	}
}

c2_status_t C2RkMppDecoder::commitBufferToMpp(std::shared_ptr<C2GraphicBlock> block)
{
	c2_status_t err = C2_OK;

	if (!block || !block->handle()) {
		ALOGE("invalid graphic block");
		return C2_BAD_VALUE;
	}

	auto c2Handle = block->handle();
	int fd = c2Handle->data[0];
	buffer_handle_t handle = nullptr;

	if (fd < 0) {
		ALOGE("invalid graphic buffer fd %d", fd);
		return C2_BAD_VALUE;
	}

	err = importGraphicBuffer(c2Handle, &handle);
	if (err != C2_OK)
		return err;

	uint32_t bufferId = (uint32_t)fd;

	OutBuffer *buffer = findOutBuffer(bufferId);
	if (buffer) {
		/* commit this buffer back to mpp */
		MppBuffer mppBuffer = buffer->mppBuffer;
		if (mppBuffer) {
			mpp_buffer_put(mppBuffer);
		}
		buffer->block = block;
		buffer->site = BUFFER_SITE_BY_MPP;

		ALOGV("put this buffer, index %u mppBuf %p", bufferId, mppBuffer);
	} else {
		/* register this buffer to mpp group */
		uint64_t allocSize = 0;
		status_t status = GraphicBufferMapper::get().getAllocationSize(handle, &allocSize);
		if (status != OK || allocSize == 0) {
			ALOGE("failed to get allocation size, status %d", status);
			freeGraphicBuffer(handle);
			return C2_BAD_VALUE;
		}

		MppBuffer mppBuffer = nullptr;
		MppBufferInfo info;
		memset(&info, 0, sizeof(info));

		info.type  = MPP_BUFFER_TYPE_ION;
		info.fd    = fd;
		info.size  = (size_t)allocSize;
		info.index = (int)bufferId;

		MPP_RET ret = mpp_buffer_import_with_tag(
			mFrmGrp, &info, &mppBuffer, "codec2", __FUNCTION__);
		if (ret != MPP_OK || mppBuffer == nullptr) {
			ALOGE("failed to import buffer, ret %d", ret);
			freeGraphicBuffer(handle);
			return C2_CORRUPTED;
		}

		OutBuffer *newBuffer = new OutBuffer;
		newBuffer->index     = bufferId;
		newBuffer->mppBuffer = mppBuffer;
		newBuffer->block     = block;
		newBuffer->site      = BUFFER_SITE_BY_MPP;

		// signal buffer available to decoder
		mpp_buffer_put(mppBuffer);

		mOutBuffers.push(newBuffer);

		ALOGV("import this buffer, index %u size %zu mppBuf %p listSize %zu",
			bufferId, info.size, mppBuffer, mOutBuffers.size());
	}

	freeGraphicBuffer(handle);

	return err;
}

c2_status_t C2RkMppDecoder::ensureDecoderState()
{
	c2_status_t err = C2_OK;

	if (isPendingFlushing()) {
		ALOGV("NO NEED ensure, in pending flush");
		return err;
	}

	Mutex::Autolock autoLock(mBufferLock);

	if (!mBlockPool) {
		return C2_BAD_STATE;
	}

	/*
	 * MPP 直接写 gralloc buffer，所以按 MPP 的 stride 申请，保证 pitch 对齐；
	 * 输出的裁剪在 createGraphicBuffer() 里用 C2Rect 完成。
	 */
	uint32_t blockW = mHorStride;
	uint32_t blockH = mVerStride;
	// TODO: Add MPP formats to Android HAL color format support.
	uint32_t format = HAL_PIXEL_FORMAT_YCrCb_NV12;
	uint64_t usage = GRALLOC_USAGE_HW_VIDEO_ENCODER;

	uint32_t target = mIntf->mActualOutputDelay->value;
	uint32_t owned = (uint32_t)getOutBufferCountOwnByMpi();
	uint32_t count = target > owned ? target - owned : 0;
	uint32_t i = 0;

	for (i = 0; i < count; i++) {
		std::shared_ptr<C2GraphicBlock> outblock;

		err = mBlockPool->fetchGraphicBlock(blockW, blockH, format,
						    C2AndroidMemoryUsage::FromGrallocUsage(usage),
						    &outblock);
		if (err != C2_OK) {
			ALOGE("failed to fetchGraphicBlock, err %d", err);
			break;
		}

		err = commitBufferToMpp(outblock);
		if (err != C2_OK) {
			ALOGE("failed to commit buffer");
			break;
		}
	}

	ALOGV("required (%ux%u) usage 0x%" PRIx64 " format 0x%x, fetch %u/%u",
		blockW, blockH, usage, format, i, count);

	return err;
}

bool C2RkMppDecoder::isPendingFlushing()
{
	return mFlushing;
}

void C2RkMppDecoder::postFrameReady()
{
	sp<AMessage> msg = new AMessage(WorkHandler::kWhatFrameReady, mHandler);
	msg->setPointer("thiz", this);
	msg->post();
}

c2_status_t C2RkMppDecoder::onFrameReady()
{
	c2_status_t err = C2_OK;

outframe:
	OutWorkEntry entry;
	memset(&entry, 0, sizeof(entry));

	err = getoutframe(&entry);
	if (err == C2_OK) {
		finishWork(entry);
		/* Avoid stock frame, continue to search available output */
		ensureDecoderState();
		goto outframe;
	} else if (err == C2_CORRUPTED) {
		ALOGE("signalling error");
		mSignalledError = true;
	}

	return err;
}

c2_status_t C2RkMppDecoder::sendpacket(uint8_t *data, size_t size, uint64_t pts, uint32_t flags)
{
	c2_status_t ret = C2_OK;
	MppPacket packet = nullptr;

	mpp_packet_init(&packet, data, size);
	mpp_packet_set_pts(packet, pts);
	mpp_packet_set_pos(packet, data);
	mpp_packet_set_length(packet, size);

	if (flags & C2FrameData::FLAG_END_OF_STREAM) {
		ALOGI("send input eos");
		mpp_packet_set_eos(packet);
	}

	if (flags & C2FrameData::FLAG_CODEC_CONFIG) {
		mpp_packet_set_extra_data(packet);
	}

	MPP_RET err = MPP_OK;
	static uint32_t kMaxRetryCnt = 1000;
	uint32_t retry = 0;

	while (true) {
		err = mMppMpi->decode_put_packet(mMppCtx, packet);
		if (err == MPP_OK) {
			ALOGV("send packet pts %" PRIu64 " size %zu", pts, size);
			break;
		}

		if ((++retry) > kMaxRetryCnt) {
			ret = C2_CORRUPTED;
			break;
		} else if (retry % 200 == 0) {
			/*
			 * FIXME:
			 * When player get paused, fetchGraphicBlock may get blocked since
			 * surface fence paused. In this case, we are not able to output
			 * frame and then the input process stucked also.
			 *
			 * To solve this issue, we attempt to send packet continuely when
			 * fetchGraphicBlock get blocked. I still don't know is there a better
			 * way to know player get paused?
			 */
			if (mBufferLock.tryLock() != NO_ERROR) {
				retry = 0;
			} else {
				ALOGW("try to resend packet, pts %" PRIu64, pts);
				mBufferLock.unlock();
			}
		}

		usleep(3 * 1000);
	}

	mpp_packet_deinit(&packet);

	return ret;
}

c2_status_t C2RkMppDecoder::getoutframe(OutWorkEntry *entry)
{
	c2_status_t ret = C2_OK;
	MPP_RET err = MPP_OK;
	MppFrame frame = nullptr;
	Mutex::Autolock autoLock(mBufferLock);

	if (isPendingFlushing()) {
		return C2_CANCELED;
	}

	err = mMppMpi->decode_get_frame(mMppCtx, &frame);
	if (err != MPP_OK || frame == nullptr) {
		return C2_NOT_FOUND;
	}

	uint32_t width   = mpp_frame_get_width(frame);
	uint32_t height  = mpp_frame_get_height(frame);
	uint32_t hstride = mpp_frame_get_hor_stride(frame);
	uint32_t vstride = mpp_frame_get_ver_stride(frame);
	uint32_t error   = mpp_frame_get_errinfo(frame);
	uint32_t eos     = mpp_frame_get_eos(frame);
	uint64_t pts     = mpp_frame_get_pts(frame);
	uint32_t flags   = 0;
	bool sawEos      = false;

	MppFrameFormat format = mpp_frame_get_fmt(frame);
	MppBuffer   mppBuffer = mpp_frame_get_buffer(frame);
	OutBuffer  *outBuffer = nullptr;

	if (mpp_frame_get_info_change(frame)) {
		ALOGI("info-change with old dimensions(%dx%d) stride(%dx%d) fmt %d",
			mWidth, mHeight, mHorStride, mVerStride, mColorFormat);
		ALOGI("info-change with new dimensions(%dx%d) stride(%dx%d) fmt %d",
			width, height, hstride, vstride, format);

		if (width > kMaxVideoWidth || height > kMaxVideoHeight) {
			ALOGE("unsupport video size %dx%d, signalled Error.", width, height);
			ret = C2_CORRUPTED;
			goto cleanUp;
		}

		clearOutBuffers();
		if (mFrmGrp) {
			mpp_buffer_group_clear(mFrmGrp);
		}

		mWidth = width;
		mHeight = height;
		mHorStride = hstride;
		mVerStride = vstride;
		mColorFormat = format;

		// All buffer group config done. Set info change ready to let
		// decoder continue decoding.
		mMppMpi->control(mMppCtx, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);

		C2StreamPictureSizeInfo::output size(0u, mWidth, mHeight);
		std::vector<std::unique_ptr<C2SettingResult>> failures;
		ret = mIntf->config({&size}, C2_MAY_BLOCK, &failures);
		if (ret != C2_OK) {
			ALOGE("failed to set width and height");
			ret = C2_CORRUPTED;
			goto cleanUp;
		}

		ret = updateOutputDelay();
		if (ret != C2_OK) {
			ALOGE("failed to update output delay");
			ret = C2_CORRUPTED;
			goto cleanUp;
		}

		// feekback config update to first output frame.
		flags |= BUFFER_FLAGS_INFO_CHANGE;

		goto cleanUp;
	}

	if (eos) {
		ALOGI("get output eos.");
		sawEos = true;
		flags |= BUFFER_FLAGS_EOS;
		// ignore null frame with eos
		if (!mppBuffer)
			goto cleanUp;
	}

	if (error) {
		ALOGW("skip error frame with pts %" PRIu64, pts);
		flags |= BUFFER_FLAGS_ERROR_FRAME;
	}

	if (isPendingFlushing()) {
		ret = C2_CANCELED;
		ALOGV("ignore frame(pts %" PRIu64 ") output since pending flush", pts);
		goto cleanUp;
	}

	outBuffer = findOutBuffer(mppBuffer);
	if (!outBuffer) {
		ret = C2_CORRUPTED;
		ALOGE("get outdated mppBuffer %p", mppBuffer);
		goto cleanUp;
	}

	ALOGV("get one frame [%u:%u] stride [%u:%u] pts %" PRIu64 " err %u index %u",
		width, height, hstride, vstride, pts, error, outBuffer->index);

	outBuffer->site = BUFFER_SITE_BY_US;
	// signal buffer occupied by users
	mpp_buffer_inc_ref(mppBuffer);

	if (mCodingType == MPP_VIDEO_CodingAVC ||
		mCodingType == MPP_VIDEO_CodingHEVC ||
		mCodingType == MPP_VIDEO_CodingAV1 ||
		mCodingType == MPP_VIDEO_CodingMPEG2) {
		getVuiParams(frame);
	}

	entry->outblock = outBuffer->block;
	entry->timestamp = pts;

cleanUp:
	entry->flags = flags;

	/*
	 * EOS 帧如果没能交给 finishWork()（例如 buffer 不在我们的列表里），
	 * 也必须唤醒等待 output eos 的调用者，否则 process()/drain() 会一直等下去。
	 */
	if (sawEos && ret != C2_OK) {
		Mutex::Autolock autoLock(mEosLock);
		mOutputEos = true;
		mEosCondition.signal();
	}

	if (frame) {
		mpp_frame_deinit(&frame);
		frame = nullptr;
	}

	return ret;
}

} // namespace codec2::rkmpp
