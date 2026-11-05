#ifndef C2_RK_MPP_DECODER_HPP
#define C2_RK_MPP_DECODER_HPP

#include <atomic>
#include <unordered_map>
#include <vector>

#include <media/stagefright/foundation/MediaDefs.h>

#include <C2RkMppComponent.hpp>
#include <C2RkMppComponentInterface.hpp>
#include <C2RkMppUtils.hpp>

namespace android {
struct ColorAspects;
}

namespace codec2::rkmpp {

/* max support video resolution */
constexpr uint32_t kMaxVideoWidth = 8192;
constexpr uint32_t kMaxVideoHeight = 4320;

constexpr size_t kMinInputBufferSize = 2 * 1024 * 1024;

class C2RkMppDecoder : public C2RkMppComponent {
public:
	class IntfImpl;

	C2RkMppDecoder(
		const char *name,
		const char *mime,
		c2_node_id_t id,
		const std::shared_ptr<IntfImpl> &intfImpl);
	virtual ~C2RkMppDecoder();

	/**
	 * Initialize internal states of the component according to the config set
	 * in the interface.
	 *
	 * This method is called during start(), but only at the first invocation or
	 * after reset().
	 */
	c2_status_t onInit() override;

	/**
	 * Stop the component.
	 */
	c2_status_t onStop() override;

	/**
	 * Reset the component.
	 */
	void onReset() override;

	/**
	 * Release the component.
	 */
	void onRelease() override;

	/**
	 * Flush the component.
	 */
	c2_status_t onFlush_sm() override;

	/**
	 * Process the given work and finish pending work using finish().
	 *
	 * \param[in,out]   work	the work to process
	 * \param[in]	   pool	the pool to use for allocating output blocks.
	 */
	void process(
		const std::unique_ptr<C2Work> &work,
		const std::shared_ptr<C2BlockPool> &pool) override;

	/**
	 * Drain the component and finish pending work using finish().
	 *
	 * \param[in]   drainMode   mode of drain.
	 * \param[in]   pool		the pool to use for allocating output blocks.
	 *
	 * \retval C2_OK			The component has drained all pending output
	 *						  work.
	 * \retval C2_OMITTED	   Unsupported mode (e.g. DRAIN_CHAIN)
	 */
	c2_status_t drain(
		uint32_t drainMode,
		const std::shared_ptr<C2BlockPool> &pool) override;

	bool isPendingFlushing();

	void postFrameReady();
	c2_status_t onFrameReady();

private:
	enum OutBufferSite {
		BUFFER_SITE_BY_MPP = 0,
		BUFFER_SITE_BY_US,
		BUFFER_SITE_BY_BUTT,
	};

	enum OutBufferFlags {
		BUFFER_FLAGS_EOS         = 0x1,
		BUFFER_FLAGS_ERROR_FRAME = 0x2,
		BUFFER_FLAGS_INFO_CHANGE = 0x4,
	};

	typedef struct {
		/* index to find this buffer */
		uint32_t       index;
		/* mpp buffer */
		MppBuffer      mppBuffer;
		/* who own this buffer */
		OutBufferSite  site;
		/* block shared by surface*/
		std::shared_ptr<C2GraphicBlock> block;
	} OutBuffer;

	typedef struct {
		std::shared_ptr<C2GraphicBlock> outblock;
		uint64_t timestamp;
		uint32_t flags;
	} OutWorkEntry;

	class WorkHandler : public AHandler {
	public:
		enum {
			kWhatFrameReady,
			kWhatFlushMessage,
		};

		WorkHandler() {}
		~WorkHandler() override = default;

		void waitAllMsgFlushed();

	protected:
		void onMessageReceived(const sp<AMessage> &msg) override;
	};

	std::shared_ptr<IntfImpl> mIntf;

	Mutex     mBufferLock;

	Mutex     mEosLock;
	Condition mEosCondition;

	sp<ALooper>     mLooper;
	sp<WorkHandler> mHandler;

	/* MPI interface parameters */
	MppCtx          mMppCtx;
	MppApi         *mMppMpi;
	MppCodingType   mCodingType;
	MppFrameFormat  mColorFormat;
	MppBufferGroup  mFrmGrp;
	Vector<OutBuffer*> mOutBuffers;

	uint32_t mWidth;
	uint32_t mHeight;
	uint32_t mHorStride;
	uint32_t mVerStride;

	bool mStarted;
	bool mFlushed;
	bool mSignalledInputEos;
	bool mOutputEos;
	std::atomic<bool> mSignalledError;
	bool mLowLatencyMode;
	std::atomic<bool> mFlushing;

	/*
	 * MPP 输出帧的 pts 到输入 work 的 frameIndex 的映射。输出帧到达时
	 * 用它找到对应的 pending work，交给基类的 finish() 完成。
	 */
	Mutex mPendingLock;
	std::unordered_map<uint64_t, uint64_t> mPtsToFrameIndex;
	std::atomic<bool> mPendingConfigUpdate;

	std::shared_ptr<C2BlockPool> mBlockPool;

	// Color aspects. These are ISO values and are meant to detect changes
	// in aspects to avoid converting them to C2 values for each frame.
	struct VuiColorAspects {
		uint8_t primaries;
		uint8_t transfer;
		uint8_t coeffs;
		uint8_t fullRange;

		// default color aspects
		VuiColorAspects()
			: primaries(2), transfer(2), coeffs(2), fullRange(0) { }

		bool operator==(const VuiColorAspects &o) const
		{
			return primaries == o.primaries && transfer == o.transfer &&
				coeffs == o.coeffs && fullRange == o.fullRange;
		}
	} mBitstreamColorAspects;

	c2_status_t setupAndStartLooper();
	void stopAndReleaseLooper();

	c2_status_t updateOutputDelay();
	void finishWork(OutWorkEntry entry);

	c2_status_t initDecoder();
	void setDefaultCodecColorAspectsIfNeeded(android::ColorAspects &aspects);
	void getVuiParams(MppFrame frame);
	c2_status_t commitBufferToMpp(std::shared_ptr<C2GraphicBlock> block);
	c2_status_t ensureDecoderState();
	c2_status_t sendpacket(uint8_t *data, size_t size, uint64_t pts, uint32_t flags);
	c2_status_t getoutframe(OutWorkEntry *entry);

	/*
	 * OutBuffer vector operations
	 */
	OutBuffer* findOutBuffer(uint32_t index)
	{
		for (size_t i = 0; i < mOutBuffers.size(); i++) {
			OutBuffer *buffer = mOutBuffers.editItemAt(i);
			if (buffer->index == index) {
				return buffer;
			}
		}
		return nullptr;
	}

	OutBuffer* findOutBuffer(MppBuffer mppBuffer)
	{
		for (size_t i = 0; i < mOutBuffers.size(); i++) {
			OutBuffer *buffer = mOutBuffers.editItemAt(i);
			if (buffer->mppBuffer == mppBuffer) {
				return buffer;
			}
		}
		return nullptr;
	}

	void clearOutBuffers()
	{
		while (!mOutBuffers.isEmpty()) {
			OutBuffer *buffer = mOutBuffers.editItemAt(0);
			if (buffer != nullptr) {
				if (buffer->site != BUFFER_SITE_BY_MPP) {
					mpp_buffer_put(buffer->mppBuffer);
				}
				delete buffer;
			}
			mOutBuffers.removeAt(0);
		}
	}

	int getOutBufferCountOwnByMpi()
	{
		int count = 0;
		for (size_t i = 0; i < mOutBuffers.size(); i++) {
			OutBuffer *buffer = mOutBuffers.editItemAt(i);
			if (buffer->site == BUFFER_SITE_BY_MPP) {
			    count++;
			}
		}
		return count;
	}

	C2_DO_NOT_COPY(C2RkMppDecoder);
};

#define C2_DEFAULT_REF_FRAME_COUNT  12
#define C2_MAX_REF_FRAME_COUNT      21

class C2RkMppDecoder::IntfImpl : public C2RkMppComponentInterface<void>::BaseParams {
public:
	explicit IntfImpl(
		const std::shared_ptr<C2ReflectorHelper> &helper,
		C2String name,
		C2Component::kind_t kind,
		C2Component::domain_t domain,
		C2String mediaType)
		: C2RkMppComponentInterface<void>::BaseParams(helper, name, kind, domain, mediaType)
	{
		addParameter(
			DefineParam(mActualOutputDelay, C2_PARAMKEY_OUTPUT_DELAY)
			.withDefault(new C2PortActualDelayTuning::output(C2_MAX_REF_FRAME_COUNT))
			.withFields({C2F(mActualOutputDelay, value).inRange(0, C2_MAX_REF_FRAME_COUNT)})
			.withSetter(Setter<decltype(*mActualOutputDelay)>::StrictValueWithNoDeps)
			.build());

		addParameter(
			DefineParam(mAttrib, C2_PARAMKEY_COMPONENT_ATTRIBUTES)
			.withConstValue(new C2ComponentAttributesSetting(C2Component::ATTRIB_IS_TEMPORAL))
			.build());

		// input picture frame size
		addParameter(
			DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
			.withDefault(new C2StreamPictureSizeInfo::output(0u, 320, 240))
			.withFields({
				C2F(mSize, width).inRange(2, kMaxVideoWidth, 2),
				C2F(mSize, height).inRange(2, kMaxVideoHeight, 2),
			})
			.withSetter(SizeSetter)
			.build());

		addParameter(
			DefineParam(mMaxSize, C2_PARAMKEY_MAX_PICTURE_SIZE)
			.withDefault(new C2StreamMaxPictureSizeTuning::output(0u, 320, 240))
			.withFields({
				C2F(mSize, width).inRange(2, kMaxVideoWidth, 2),
				C2F(mSize, height).inRange(2, kMaxVideoHeight, 2),
			})
			.withSetter(MaxPictureSizeSetter, mSize)
			.build());

		addParameter(
			DefineParam(mFrameRate, C2_PARAMKEY_FRAME_RATE)
			.withDefault(new C2StreamFrameRateInfo::output(0u, 1.))
			// TODO: More restriction?
			.withFields({C2F(mFrameRate, value).greaterThan(0.)})
			.withSetter(Setter<decltype(*mFrameRate)>::StrictValueWithNoDeps)
			.build());

		addParameter(
			DefineParam(mBlockSize, C2_PARAMKEY_BLOCK_SIZE)
			.withDefault(new C2StreamBlockSizeInfo::output(0u, 320, 240))
			.withFields({
				C2F(mBlockSize, width).inRange(2, kMaxVideoWidth, 2),
				C2F(mBlockSize, height).inRange(2, kMaxVideoHeight, 2),
			})
			.withSetter(BlockSizeSetter)
			.build());

		std::vector<uint32_t> pixelFormats = {HAL_PIXEL_FORMAT_YCBCR_420_888};

		// TODO: support more formats?
		addParameter(
			DefineParam(mPixelFormat, C2_PARAMKEY_PIXEL_FORMAT)
			.withDefault(new C2StreamPixelFormatInfo::output(
				0u, HAL_PIXEL_FORMAT_YCBCR_420_888))
			.withFields({C2F(mPixelFormat, value).oneOf(pixelFormats)})
			.withSetter((Setter<decltype(*mPixelFormat)>::StrictValueWithNoDeps))
			.build());

		// profile and level
		if (mediaType == MEDIA_MIMETYPE_VIDEO_AVC) {
			std::vector<uint32_t> avcProfiles = {
				C2Config::PROFILE_AVC_CONSTRAINED_BASELINE,
				C2Config::PROFILE_AVC_BASELINE,
				C2Config::PROFILE_AVC_MAIN,
				C2Config::PROFILE_AVC_CONSTRAINED_HIGH,
				C2Config::PROFILE_AVC_PROGRESSIVE_HIGH,
				C2Config::PROFILE_AVC_HIGH
			};
			addParameter(
				DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
				.withDefault(new C2StreamProfileLevelInfo::input(0u,
					C2Config::PROFILE_AVC_BASELINE, C2Config::LEVEL_AVC_5_1))
				.withFields({
					C2F(mProfileLevel, profile).oneOf(avcProfiles),
					C2F(mProfileLevel, level).oneOf({
						C2Config::LEVEL_AVC_1, C2Config::LEVEL_AVC_1B, C2Config::LEVEL_AVC_1_1,
						C2Config::LEVEL_AVC_1_2, C2Config::LEVEL_AVC_1_3,
						C2Config::LEVEL_AVC_2, C2Config::LEVEL_AVC_2_1, C2Config::LEVEL_AVC_2_2,
						C2Config::LEVEL_AVC_3, C2Config::LEVEL_AVC_3_1, C2Config::LEVEL_AVC_3_2,
						C2Config::LEVEL_AVC_4, C2Config::LEVEL_AVC_4_1, C2Config::LEVEL_AVC_4_2,
						C2Config::LEVEL_AVC_5, C2Config::LEVEL_AVC_5_1, C2Config::LEVEL_AVC_5_2,
						C2Config::LEVEL_AVC_6, C2Config::LEVEL_AVC_6_1, C2Config::LEVEL_AVC_6_2})
				})
				.withSetter(ProfileLevelSetter, mSize)
				.build());
		} else if (mediaType == MEDIA_MIMETYPE_VIDEO_HEVC) {
			std::vector<uint32_t> hevcProfiles = {C2Config::PROFILE_HEVC_MAIN};
			addParameter(
				DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
				.withDefault(new C2StreamProfileLevelInfo::input(0u,
					C2Config::PROFILE_HEVC_MAIN, C2Config::LEVEL_HEVC_MAIN_5_1))
				.withFields({
					C2F(mProfileLevel, profile).oneOf(hevcProfiles),
					C2F(mProfileLevel, level).oneOf({
						C2Config::LEVEL_HEVC_MAIN_1,
						C2Config::LEVEL_HEVC_MAIN_2, C2Config::LEVEL_HEVC_MAIN_2_1,
						C2Config::LEVEL_HEVC_MAIN_3, C2Config::LEVEL_HEVC_MAIN_3_1,
						C2Config::LEVEL_HEVC_MAIN_4, C2Config::LEVEL_HEVC_MAIN_4_1,
						C2Config::LEVEL_HEVC_MAIN_5, C2Config::LEVEL_HEVC_MAIN_5_1,
						C2Config::LEVEL_HEVC_MAIN_5_2, C2Config::LEVEL_HEVC_MAIN_6,
						C2Config::LEVEL_HEVC_MAIN_6_1, C2Config::LEVEL_HEVC_MAIN_6_2,
						C2Config::LEVEL_HEVC_HIGH_4, C2Config::LEVEL_HEVC_HIGH_4_1,
						C2Config::LEVEL_HEVC_HIGH_5, C2Config::LEVEL_HEVC_HIGH_5_1,
						C2Config::LEVEL_HEVC_HIGH_5_2, C2Config::LEVEL_HEVC_HIGH_6,
						C2Config::LEVEL_HEVC_HIGH_6_1, C2Config::LEVEL_HEVC_HIGH_6_2})
				})
				.withSetter(ProfileLevelSetter, mSize)
				.build());
		} else if (mediaType == MEDIA_MIMETYPE_VIDEO_MPEG2) {
			addParameter(
				DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
				.withDefault(new C2StreamProfileLevelInfo::input(0u,
					C2Config::PROFILE_MP2V_SIMPLE, C2Config::LEVEL_MP2V_HIGH))
				.withFields({
					C2F(mProfileLevel, profile).oneOf({
						C2Config::PROFILE_MP2V_SIMPLE,
						C2Config::PROFILE_MP2V_MAIN}),
					C2F(mProfileLevel, level).oneOf({
						C2Config::LEVEL_MP2V_LOW,
						C2Config::LEVEL_MP2V_MAIN,
						C2Config::LEVEL_MP2V_HIGH_1440,
						C2Config::LEVEL_MP2V_HIGH})
				})
				.withSetter(ProfileLevelSetter, mSize)
				.build());
		} else if (mediaType == MEDIA_MIMETYPE_VIDEO_MPEG4) {
			addParameter(
				DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
				.withDefault(new C2StreamProfileLevelInfo::input(0u,
					C2Config::PROFILE_MP4V_SIMPLE, C2Config::LEVEL_MP4V_3))
				.withFields({
					C2F(mProfileLevel, profile).oneOf({
						C2Config::PROFILE_MP4V_SIMPLE}),
					C2F(mProfileLevel, level).oneOf({
						C2Config::LEVEL_MP4V_0,
						C2Config::LEVEL_MP4V_0B,
						C2Config::LEVEL_MP4V_1,
						C2Config::LEVEL_MP4V_2,
						C2Config::LEVEL_MP4V_3})
				})
				.withSetter(ProfileLevelSetter, mSize)
				.build());
		} else if (mediaType == MEDIA_MIMETYPE_VIDEO_H263) {
			addParameter(
				DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
				.withDefault(new C2StreamProfileLevelInfo::input(0u,
					C2Config::PROFILE_H263_BASELINE, C2Config::LEVEL_H263_30))
				.withFields({
					C2F(mProfileLevel, profile).oneOf({
						C2Config::PROFILE_H263_BASELINE,
						C2Config::PROFILE_H263_ISWV2}),
					C2F(mProfileLevel, level).oneOf({
						C2Config::LEVEL_H263_10,
						C2Config::LEVEL_H263_20,
						C2Config::LEVEL_H263_30,
						C2Config::LEVEL_H263_40,
						C2Config::LEVEL_H263_45})
				})
				.withSetter(ProfileLevelSetter, mSize)
				.build());
		} else if (mediaType == MEDIA_MIMETYPE_VIDEO_VP9) {
			std::vector<uint32_t> vp9Profiles ={C2Config::PROFILE_VP9_0};
			addParameter(
				DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
				.withDefault(new C2StreamProfileLevelInfo::input(0u,
					C2Config::PROFILE_VP9_0, C2Config::LEVEL_VP9_5))
				.withFields({
					C2F(mProfileLevel, profile).oneOf(vp9Profiles),
					C2F(mProfileLevel, level).oneOf({
						C2Config::LEVEL_VP9_1,
						C2Config::LEVEL_VP9_1_1,
						C2Config::LEVEL_VP9_2,
						C2Config::LEVEL_VP9_2_1,
						C2Config::LEVEL_VP9_3,
						C2Config::LEVEL_VP9_3_1,
						C2Config::LEVEL_VP9_4,
						C2Config::LEVEL_VP9_4_1,
						C2Config::LEVEL_VP9_5,
						C2Config::LEVEL_VP9_5_1,
						C2Config::LEVEL_VP9_5_2,
						C2Config::LEVEL_VP9_6,
						C2Config::LEVEL_VP9_6_1,
						C2Config::LEVEL_VP9_6_2})
				})
				.withSetter(ProfileLevelSetter, mSize)
				.build());
		} else if (mediaType == MEDIA_MIMETYPE_VIDEO_AV1) {
			addParameter(
				DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
				.withDefault(new C2StreamProfileLevelInfo::input(0u,
					C2Config::PROFILE_AV1_0, C2Config::LEVEL_AV1_6_3))
				.withFields({
					C2F(mProfileLevel, profile).oneOf({
						C2Config::PROFILE_AV1_0,
						C2Config::PROFILE_AV1_1}),
					C2F(mProfileLevel, level).oneOf({
						C2Config::LEVEL_AV1_2, C2Config::LEVEL_AV1_2_1, C2Config::LEVEL_AV1_2_2,
						C2Config::LEVEL_AV1_2_3, C2Config::LEVEL_AV1_3, C2Config::LEVEL_AV1_3_1,
						C2Config::LEVEL_AV1_3_2, C2Config::LEVEL_AV1_3_3, C2Config::LEVEL_AV1_4,
						C2Config::LEVEL_AV1_4_1, C2Config::LEVEL_AV1_4_2, C2Config::LEVEL_AV1_4_3,
						C2Config::LEVEL_AV1_5, C2Config::LEVEL_AV1_5_1, C2Config::LEVEL_AV1_5_2,
						C2Config::LEVEL_AV1_5_3, C2Config::LEVEL_AV1_6, C2Config::LEVEL_AV1_6_1,
						C2Config::LEVEL_AV1_6_2, C2Config::LEVEL_AV1_6_3})
				})
				.withSetter(ProfileLevelSetter, mSize)
				.build());
		}

		// max input buffer size
		addParameter(
			DefineParam(mMaxInputSize, C2_PARAMKEY_INPUT_MAX_BUFFER_SIZE)
			.withDefault(new C2StreamMaxBufferSizeInfo::input(0u, kMinInputBufferSize))
			.withFields({
				C2F(mMaxInputSize, value).any(),
			})
			.calculatedAs(MaxInputSizeSetter, mMaxSize)
			.build());

		// ColorInfo
		helper->addStructDescriptors<C2ChromaOffsetStruct>();
		std::shared_ptr<C2StreamColorInfo::output> defaultColorInfo =
			C2StreamColorInfo::output::AllocShared(
				{ C2ChromaOffsetStruct::ITU_YUV_420_0() },
				0u, 8u /* bitDepth */, C2Color::YUV_420);

		addParameter(
			DefineParam(mColorInfo, C2_PARAMKEY_CODED_COLOR_INFO)
			.withConstValue(defaultColorInfo)
			.build());

		// colorAspects
		addParameter(
			DefineParam(mDefaultColorAspects, C2_PARAMKEY_DEFAULT_COLOR_ASPECTS)
			.withDefault(new C2StreamColorAspectsTuning::output(
				0u, C2Color::RANGE_UNSPECIFIED, C2Color::PRIMARIES_UNSPECIFIED,
				C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
			.withFields({
				C2F(mDefaultColorAspects, range).inRange(
					C2Color::RANGE_UNSPECIFIED,     C2Color::RANGE_OTHER),
				C2F(mDefaultColorAspects, primaries).inRange(
					C2Color::PRIMARIES_UNSPECIFIED, C2Color::PRIMARIES_OTHER),
				C2F(mDefaultColorAspects, transfer).inRange(
					C2Color::TRANSFER_UNSPECIFIED,  C2Color::TRANSFER_OTHER),
				C2F(mDefaultColorAspects, matrix).inRange(
					C2Color::MATRIX_UNSPECIFIED,    C2Color::MATRIX_OTHER)
			})
			.withSetter(DefaultColorAspectsSetter)
			.build());

		// vui colorAspects
		if (mediaType == MEDIA_MIMETYPE_VIDEO_AVC ||
			mediaType == MEDIA_MIMETYPE_VIDEO_HEVC ||
			mediaType == MEDIA_MIMETYPE_VIDEO_AV1 ||
			mediaType == MEDIA_MIMETYPE_VIDEO_MPEG2) {
			addParameter(
				DefineParam(mCodedColorAspects, C2_PARAMKEY_VUI_COLOR_ASPECTS)
				.withDefault(new C2StreamColorAspectsInfo::input(
					0u, C2Color::RANGE_LIMITED, C2Color::PRIMARIES_UNSPECIFIED,
					C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
				.withFields({
					C2F(mCodedColorAspects, range).inRange(
						C2Color::RANGE_UNSPECIFIED,     C2Color::RANGE_OTHER),
					C2F(mCodedColorAspects, primaries).inRange(
						C2Color::PRIMARIES_UNSPECIFIED, C2Color::PRIMARIES_OTHER),
					C2F(mCodedColorAspects, transfer).inRange(
						C2Color::TRANSFER_UNSPECIFIED,  C2Color::TRANSFER_OTHER),
					C2F(mCodedColorAspects, matrix).inRange(
						C2Color::MATRIX_UNSPECIFIED,    C2Color::MATRIX_OTHER)
				})
				.withSetter(CodedColorAspectsSetter)
				.build());

		addParameter(
			DefineParam(mColorAspects, C2_PARAMKEY_COLOR_ASPECTS)
			.withDefault(new C2StreamColorAspectsInfo::output(
				0u, C2Color::RANGE_UNSPECIFIED, C2Color::PRIMARIES_UNSPECIFIED,
				C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
			.withFields({
				C2F(mColorAspects, range).inRange(
					C2Color::RANGE_UNSPECIFIED,     C2Color::RANGE_OTHER),
				C2F(mColorAspects, primaries).inRange(
					C2Color::PRIMARIES_UNSPECIFIED, C2Color::PRIMARIES_OTHER),
				C2F(mColorAspects, transfer).inRange(
					C2Color::TRANSFER_UNSPECIFIED,  C2Color::TRANSFER_OTHER),
				C2F(mColorAspects, matrix).inRange(
					C2Color::MATRIX_UNSPECIFIED,    C2Color::MATRIX_OTHER)
			})
			.withSetter(ColorAspectsSetter, mDefaultColorAspects, mCodedColorAspects)
			.build());

		addParameter(
			DefineParam(mLowLatency, C2_PARAMKEY_LOW_LATENCY_MODE)
			.withDefault(new C2GlobalLowLatencyModeTuning(false))
			.withFields({C2F(mLowLatency, value)})
			.withSetter(Setter<decltype(*mLowLatency)>::NonStrictValueWithNoDeps)
			.build());

		/* extend parameter definition */
		//addParameter(
		//	DefineParam(mDisableDpbCheck, C2_PARAMKEY_DISABLE_DPB_CHECK)
		//	.withDefault(new C2StreamDisableDpbCheck::input(0))
		//	.withFields({C2F(mDisableDpbCheck, value).any()})
		//	.withSetter(Setter<decltype(mDisableDpbCheck)::element_type>::StrictValueWithNoDeps)
		//	.build());

		//addParameter(
		//	DefineParam(mMlvecParams->driverInfo, C2_PARAMKEY_MLVEC_DEC_DRI_VERSION)
		//	.withConstValue(new C2DriverVersion::output(MLVEC_DRIVER_VERSION))
		//	.build());

		//addParameter(
		//	DefineParam(mMlvecParams->lowLatencyMode, C2_PARAMKEY_MLVEC_DEC_LOW_LATENCY_MODE)
		//	.withDefault(new C2LowLatencyMode::output(0))
		//	.withFields({
		//		C2F(mMlvecParams->lowLatencyMode, enable).any(),
		//	})
		//	.withSetter(MLowLatenctyModeSetter)
		//	.build());
		}
	}

	static C2R SizeSetter(bool mayBlock, const C2P<C2StreamPictureSizeInfo::output> &oldMe,
			      C2P<C2StreamPictureSizeInfo::output> &me)
	{
		(void)mayBlock;
		C2R res = C2R::Ok();
		if (!me.F(me.v.width).supportsAtAll(me.v.width)) {
			res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.width)));
			me.set().width = oldMe.v.width;
		}
		if (!me.F(me.v.height).supportsAtAll(me.v.height)) {
			res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.height)));
			me.set().height = oldMe.v.height;
		}
		if (me.set().width * me.set().height > kMaxVideoWidth * kMaxVideoHeight) {
			ALOGW("max support video resolution %dx%d, cur %dx%d",
				kMaxVideoWidth, kMaxVideoHeight, me.set().width, me.set().height);
		}
		return res;
	}

	static C2R MaxPictureSizeSetter(bool mayBlock, C2P<C2StreamMaxPictureSizeTuning::output> &me,
	 				const C2P<C2StreamPictureSizeInfo::output> &size)
	{
		(void)mayBlock;
		// TODO: get max width/height from the size's field helpers vs. hardcoding
		me.set().width = c2_min(c2_max(me.v.width, size.v.width), kMaxVideoWidth);
		me.set().height = c2_min(c2_max(me.v.height, size.v.height), kMaxVideoHeight);
		if (me.set().width * me.set().height > kMaxVideoWidth * kMaxVideoHeight) {
			ALOGW("max support video resolution %dx%d, cur %dx%d",
				kMaxVideoWidth, kMaxVideoHeight, me.set().width, me.set().height);
		}
		return C2R::Ok();
	}

	static C2R BlockSizeSetter(bool mayBlock, const C2P<C2StreamBlockSizeInfo::output> &oldMe,
				   C2P<C2StreamBlockSizeInfo::output> &me)
	{
		(void)mayBlock;
		C2R res = C2R::Ok();
		if (!me.F(me.v.width).supportsAtAll(me.v.width)) {
			res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.width)));
			me.set().width = oldMe.v.width;
		}
		if (!me.F(me.v.height).supportsAtAll(me.v.height)) {
			res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.height)));
			me.set().height = oldMe.v.height;
		}
		return res;
	}

	static C2R ProfileLevelSetter(bool mayBlock, C2P<C2StreamProfileLevelInfo::input> &me,
				      const C2P<C2StreamPictureSizeInfo::output> &size)
	{
		(void)mayBlock;
		(void)size;
		(void)me;  // TODO: validate
		return C2R::Ok();
	}

	static C2R MaxInputSizeSetter(bool mayBlock, C2P<C2StreamMaxBufferSizeInfo::input> &me,
				      const C2P<C2StreamMaxPictureSizeTuning::output> &maxSize)
	{
		(void)mayBlock;
		// assume compression ratio of 2
		me.set().value = c2_max((((maxSize.v.width + 63) / 64)
			* ((maxSize.v.height + 63) / 64) * 3072), kMinInputBufferSize);
		return C2R::Ok();
	}


	static C2R DefaultColorAspectsSetter(bool mayBlock, C2P<C2StreamColorAspectsTuning::output> &me)
	{
		(void)mayBlock;
		if (me.v.range > C2Color::RANGE_OTHER) {
			me.set().range = C2Color::RANGE_OTHER;
		}
		if (me.v.primaries > C2Color::PRIMARIES_OTHER) {
			me.set().primaries = C2Color::PRIMARIES_OTHER;
		}
		if (me.v.transfer > C2Color::TRANSFER_OTHER) {
			me.set().transfer = C2Color::TRANSFER_OTHER;
		}
		if (me.v.matrix > C2Color::MATRIX_OTHER) {
			me.set().matrix = C2Color::MATRIX_OTHER;
		}
		return C2R::Ok();
	}

	static C2R CodedColorAspectsSetter(bool mayBlock, C2P<C2StreamColorAspectsInfo::input> &me)
	{
		(void)mayBlock;
		if (me.v.range > C2Color::RANGE_OTHER) {
			me.set().range = C2Color::RANGE_OTHER;
		}
		if (me.v.primaries > C2Color::PRIMARIES_OTHER) {
			me.set().primaries = C2Color::PRIMARIES_OTHER;
		}
		if (me.v.transfer > C2Color::TRANSFER_OTHER) {
			me.set().transfer = C2Color::TRANSFER_OTHER;
		}
		if (me.v.matrix > C2Color::MATRIX_OTHER) {
			me.set().matrix = C2Color::MATRIX_OTHER;
		}
		return C2R::Ok();
	}

	static C2R ColorAspectsSetter(bool mayBlock, C2P<C2StreamColorAspectsInfo::output> &me,
				      const C2P<C2StreamColorAspectsTuning::output> &def,
				      const C2P<C2StreamColorAspectsInfo::input> &coded)
	{
		(void)mayBlock;
		// take default values for all unspecified fields, and coded values for specified ones
		me.set().range = coded.v.range == RANGE_UNSPECIFIED ? def.v.range : coded.v.range;
		me.set().primaries = coded.v.primaries == PRIMARIES_UNSPECIFIED
			? def.v.primaries : coded.v.primaries;
		me.set().transfer = coded.v.transfer == TRANSFER_UNSPECIFIED
			? def.v.transfer : coded.v.transfer;
		me.set().matrix = coded.v.matrix == MATRIX_UNSPECIFIED ? def.v.matrix : coded.v.matrix;
		return C2R::Ok();
	}

	//static C2R MLowLatenctyModeSetter(
	//	bool mayBlock, C2P<C2LowLatencyMode::output> &me)
	//{
	//	(void)mayBlock;
	//	(void)me;
	//	return C2R::Ok();
	//}

	std::shared_ptr<C2StreamPictureSizeInfo::output> getSize_l()
	{
		return mSize;
	}

	std::shared_ptr<C2StreamFrameRateInfo::output> getFrameRate_l()
	{
		return mFrameRate;
	}

	std::shared_ptr<C2StreamColorAspectsInfo::output> getColorAspects_l()
	{
		return mColorAspects;
	}

	std::shared_ptr<C2StreamColorAspectsTuning::output> getDefaultColorAspects_l()
	{
		return mDefaultColorAspects;
	}

	std::shared_ptr<C2GlobalLowLatencyModeTuning> getLowLatency_l()
	{
		return mLowLatency;
	}

	//std::shared_ptr<C2StreamDisableDpbCheck::input> getDisableDpbCheck_l() const
	//{
	//	return mDisableDpbCheck;
	//}

	bool getIsLowLatencyMode()
	{
		if (mLowLatency && mLowLatency->value > 0) {
			return true;
		}
		return false;
	}

private:
	std::shared_ptr<C2StreamPictureSizeInfo::output> mSize;
	std::shared_ptr<C2StreamMaxPictureSizeTuning::output> mMaxSize;
	std::shared_ptr<C2StreamFrameRateInfo::output> mFrameRate;
	std::shared_ptr<C2StreamBlockSizeInfo::output> mBlockSize;
	std::shared_ptr<C2StreamPixelFormatInfo::output> mPixelFormat;
	std::shared_ptr<C2StreamProfileLevelInfo::input> mProfileLevel;
	std::shared_ptr<C2StreamMaxBufferSizeInfo::input> mMaxInputSize;
	std::shared_ptr<C2StreamColorInfo::output> mColorInfo;
	std::shared_ptr<C2StreamColorAspectsTuning::output> mDefaultColorAspects;
	std::shared_ptr<C2StreamColorAspectsInfo::input> mCodedColorAspects;
	std::shared_ptr<C2StreamColorAspectsInfo::output> mColorAspects;
	//std::shared_ptr<C2StreamDisableDpbCheck::input> mDisableDpbCheck;
	std::shared_ptr<C2GlobalLowLatencyModeTuning> mLowLatency;
};

} // namespace codec2::rkmpp

#endif // C2_RK_MPP_DECODER_HPP
