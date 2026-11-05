#ifndef C2_RK_MPP_ENCODER_HPP
#define C2_RK_MPP_ENCODER_HPP

#include <vector>

#include <media/stagefright/foundation/MediaDefs.h>

#include <C2RkMppComponent.hpp>
#include <C2RkMppComponentInterface.hpp>
#include <C2RkMppUtils.hpp>

namespace codec2::rkmpp {

//class C2RkMppEncoderInterface : public C2RkMppComponentInterface<void> {

//};

class C2RkMppEncoder : public C2RkMppComponent {
public:
	class IntfImpl;

	C2RkMppEncoder(
		const char *name,
		const char *mime,
		c2_node_id_t id,
		const std::shared_ptr<IntfImpl> &intfImpl);
	virtual ~C2RkMppEncoder();

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

	// for derived classes
	/**
	 * Finish pending work.
	 *
	 * This method will retrieve the pending work according to |frameIndex| and
	 * feed the work into |fillWork| function. |fillWork| must be
	 * "non-blocking". Once |fillWork| returns the filled work will be returned
	 * to the client.
	 *
	 * \param[in]   frameIndex	the index of the pending work
	 * \param[in]   fillWork	  the function to fill the retrieved work.
	 */
	//void finish(uint64_t frameIndex, std::function<void(const std::unique_ptr<C2Work> &)> fillWork) override;

private:
	/* DMA buffer memery */
	typedef struct {
		int      fd;
		size_t   size;
		void    *handler; /* buffer_handle_t */
	} MyDmaBuffer_t;

	/* Supported lists for InputFormat */
	typedef enum {
		C2_INPUT_FMT_UNKNOWN = 0,
		C2_INPUT_FMT_YUV420SP,
		C2_IPNUT_FMT_RGBA,
	} MyInputFormat;

	typedef struct {
		MppPacket outPacket;
		uint64_t  frameIndex;
	} OutWorkEntry;

	const char* mName;
	const char* mMime;
	std::shared_ptr<IntfImpl> mIntf;
	MyDmaBuffer_t *mDmaMem;

	/* MPI interface parameters */
	MppCtx         mMppCtx;
	MppApi        *mMppMpi;
	MppEncCfg      mEncCfg;
	MppCodingType  mCodingType;
	MppFrameFormat mInputMppFmt;
	int32_t        mChipType;

	bool           mStarted;
	bool           mSpsPpsHeaderReceived;
	bool           mSawInputEOS;
	bool           mOutputEOS;
	bool           mSignalledError;
	int32_t        mHorStride;
	int32_t        mVerStride;
	int32_t        mCurLayerCount;
	int32_t        mInputCount;

	std::shared_ptr<C2StreamPictureSizeInfo::input> mSize;
	std::shared_ptr<C2StreamBitrateInfo::output> mBitrate;
	std::shared_ptr<C2StreamFrameRateInfo::output> mFrameRate;

	c2_status_t setupBaseCodec(void);
	c2_status_t setupBitRate(void);
	c2_status_t setupFrameRate(void);
	c2_status_t setupEncCfg(void);
	c2_status_t handleCommonDynamicCfg(void);
	c2_status_t handleRequestSyncFrame(void);
	c2_status_t initEncoder(void);

	bool isColorFmtConvertNeeded(uint32_t width, uint32_t height, MppFrameFormat fmt);
	c2_status_t getInBufferFromWork(
		const std::unique_ptr<C2Work> &work, MyDmaBuffer_t *outBuffer);
	c2_status_t sendFrame(
		MyDmaBuffer_t dBuffer, uint64_t pts, uint32_t flags);
	c2_status_t getOutPacket(OutWorkEntry *entry);

	void fillEmptyWork(const std::unique_ptr<C2Work>& work);
	void finishWork(
		const std::unique_ptr<C2Work> &work,
		const std::shared_ptr<C2BlockPool>& pool,
		OutWorkEntry entry);
};

void ParseGop(const C2StreamGopTuning::output &gop,
	      uint32_t *syncInterval, uint32_t *iInterval, uint32_t *maxBframes);

uint32_t getMppBitrateMode(int32_t mode, bool c2Type);

class C2RkMppEncoder::IntfImpl : public C2RkMppComponentInterface<void>::BaseParams {
public:
	explicit IntfImpl(
		const std::shared_ptr<C2ReflectorHelper> &helper,
		C2String name,
		C2Component::kind_t kind,
		C2Component::domain_t domain,
		C2String mediaType
	) : C2RkMppComponentInterface<void>::BaseParams(
		helper,
		name,
		kind,
		domain,
		mediaType
	)
	{
		noPrivateBuffers(); // TODO: account for our buffers here
		noInputReferences();
		noOutputReferences();
		noTimeStretch();
		setDerivedInstance(this);

		addParameter(
			DefineParam(mUsage, C2_PARAMKEY_INPUT_STREAM_USAGE)
			.withConstValue(new C2StreamUsageTuning::input(
				0u, GRALLOC_USAGE_HW_VIDEO_ENCODER)) // RK_GRALLOC_USAGE_SPECIFY_STRIDE
			.build());

		addParameter(
			DefineParam(mAttrib, C2_PARAMKEY_COMPONENT_ATTRIBUTES)
			.withConstValue(new C2ComponentAttributesSetting(
				C2Component::ATTRIB_IS_TEMPORAL))
			.build());
	
		addParameter(
			DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
			.withDefault(new C2StreamPictureSizeInfo::input(0u, 176, 144))
			.withFields({
				C2F(mSize, width).inRange(90, 7680, 2),
				C2F(mSize, height).inRange(90, 7680, 2),
			})
			.withSetter(SizeSetter)
			.build());

		addParameter(
			DefineParam(mGop, C2_PARAMKEY_GOP)
			.withDefault(C2StreamGopTuning::output::AllocShared(
					0 /* flexCount */, 0u /* stream */))
			.withFields({C2F(mGop, m.values[0].type_).any(),
					 C2F(mGop, m.values[0].count).any()})
			.withSetter(GopSetter)
			.build());

		addParameter(
			DefineParam(mRotation, C2_PARAMKEY_ROTATION)
			.withDefault(new C2StreamRotationInfo::output(0u, 0))
			.withFields({C2F(mRotation, flip).any(),
					 C2F(mRotation, value).any()})
			.withSetter(RotationSetter)
			.build());

		addParameter(
			DefineParam(mPictureQuantization, C2_PARAMKEY_PICTURE_QUANTIZATION)
			.withDefault(C2StreamPictureQuantizationTuning::output::AllocShared(
				0 /* flexCount */, 0u /* stream */))
			.withFields({
				C2F(mPictureQuantization, m.values[0].type_).oneOf(
					{C2Config::picture_type_t(I_FRAME),
					 C2Config::picture_type_t(P_FRAME),
					 C2Config::picture_type_t(B_FRAME)}),
				C2F(mPictureQuantization, m.values[0].min).any(),
				C2F(mPictureQuantization, m.values[0].max).any()})
			.withSetter(PictureQuantizationSetter)
			.build());

		addParameter(
			DefineParam(mActualInputDelay, C2_PARAMKEY_INPUT_DELAY)
			.withDefault(new C2PortActualDelayTuning::input(0))
			.withFields({C2F(mActualInputDelay, value).inRange(0, 2)})
			.calculatedAs(InputDelaySetter, mGop)
			.build());

		addParameter(
			DefineParam(mFrameRate, C2_PARAMKEY_FRAME_RATE)
			.withDefault(new C2StreamFrameRateInfo::output(0u, 1.))
			// TODO: More restriction?
			.withFields({C2F(mFrameRate, value).greaterThan(0.)})
			.withSetter(Setter<decltype(*mFrameRate)>::StrictValueWithNoDeps)
			.build());

		addParameter(
			DefineParam(mFrameRate, C2_PARAMKEY_FRAME_RATE)
			.withDefault(new C2StreamFrameRateInfo::output(0u, 1.))
			// TODO: More restriction?
			.withFields({C2F(mFrameRate, value).greaterThan(0.)})
			.withSetter(Setter<decltype(*mFrameRate)>::StrictValueWithNoDeps)
			.build());

		addParameter(
			DefineParam(mBitrateMode, C2_PARAMKEY_BITRATE_MODE)
			.withDefault(new C2StreamBitrateModeTuning::output(
				0u, C2Config::BITRATE_VARIABLE))
			.withFields({
				C2F(mBitrateMode, value).oneOf({
				C2Config::BITRATE_CONST,
				C2Config::BITRATE_VARIABLE,
				C2Config::BITRATE_IGNORE})
			})
			.withSetter(
				Setter<decltype(*mBitrateMode)>::StrictValueWithNoDeps)
			.build());

		addParameter(
			DefineParam(mBitrate, C2_PARAMKEY_BITRATE)
			.withDefault(new C2StreamBitrateInfo::output(0u, 64000))
			.withFields({C2F(mBitrate, value).inRange(4096, 10000000)})
			.withSetter(BitrateSetter)
			.build());

		if (mediaType == MEDIA_MIMETYPE_VIDEO_AVC) {
			addParameter(
				DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
				.withDefault(new C2StreamProfileLevelInfo::output(
					0u, PROFILE_AVC_BASELINE, LEVEL_AVC_3_1))
				.withFields({
					C2F(mProfileLevel, profile).oneOf({
						PROFILE_AVC_BASELINE, PROFILE_AVC_MAIN, PROFILE_AVC_HIGH,
					}),
					C2F(mProfileLevel, level).oneOf({
						LEVEL_AVC_1,   LEVEL_AVC_1B,  LEVEL_AVC_1_1,  LEVEL_AVC_1_2,
						LEVEL_AVC_1_3, LEVEL_AVC_2,   LEVEL_AVC_2_1,  LEVEL_AVC_2_2,
						LEVEL_AVC_3,   LEVEL_AVC_3_1, LEVEL_AVC_3_2,  LEVEL_AVC_4,
						LEVEL_AVC_4_1, LEVEL_AVC_4_2, LEVEL_AVC_5,    LEVEL_AVC_5_1,
					}),
				})
				.withSetter(AVCProfileLevelSetter, mSize, mFrameRate, mBitrate)
				.build());
		} else if (mediaType == MEDIA_MIMETYPE_VIDEO_HEVC) {
			addParameter(
				DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
				.withDefault(new C2StreamProfileLevelInfo::output(
					0u, PROFILE_HEVC_MAIN, LEVEL_HEVC_MAIN_4_1))
				.withFields({
					C2F(mProfileLevel, profile).oneOf({
						PROFILE_HEVC_MAIN,
					}),
					C2F(mProfileLevel, level).oneOf({
						LEVEL_HEVC_MAIN_1,   LEVEL_HEVC_MAIN_2,   LEVEL_HEVC_MAIN_2_1,
						LEVEL_HEVC_MAIN_3,   LEVEL_HEVC_MAIN_3_1, LEVEL_HEVC_MAIN_4,
						LEVEL_HEVC_MAIN_4_1, LEVEL_HEVC_MAIN_5,   LEVEL_HEVC_MAIN_5_1,
					}),
				})
				.withSetter(HEVCProfileLevelSetter, mSize, mFrameRate, mBitrate)
				.build());
		} else {
			addParameter(
				DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
				.withDefault(new C2StreamProfileLevelInfo::output(
					0u, PROFILE_UNUSED, LEVEL_UNUSED))
				.withFields({
					C2F(mProfileLevel, profile).any(),
					C2F(mProfileLevel, level).any(),
				})
				.withSetter(DefaultProfileLevelSetter, mSize, mFrameRate, mBitrate)
				.build());
		}

		addParameter(
			DefineParam(mRequestSync, C2_PARAMKEY_REQUEST_SYNC_FRAME)
			.withDefault(new C2StreamRequestSyncFrameTuning::output(0u, C2_FALSE))
			.withFields({C2F(mRequestSync, value).oneOf({ C2_FALSE, C2_TRUE }) })
			.withSetter(Setter<decltype(*mRequestSync)>::NonStrictValueWithNoDeps)
			.build());

		addParameter(
			DefineParam(mSyncFramePeriod, C2_PARAMKEY_SYNC_FRAME_INTERVAL)
			.withDefault(new C2StreamSyncFrameIntervalTuning::output(0u, 1000000))
			.withFields({C2F(mSyncFramePeriod, value).any()})
			.withSetter(Setter<decltype(*mSyncFramePeriod)>::StrictValueWithNoDeps)
			.build());

		addParameter(
			DefineParam(mColorAspects, C2_PARAMKEY_COLOR_ASPECTS)
			.withDefault(new C2StreamColorAspectsInfo::input(
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
			.withSetter(ColorAspectsSetter)
			.build());

		addParameter(
			DefineParam(mCodedColorAspects, C2_PARAMKEY_VUI_COLOR_ASPECTS)
			.withDefault(new C2StreamColorAspectsInfo::output(
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
			.withSetter(CodedColorAspectsSetter, mColorAspects)
			.build());

		addParameter(
			DefineParam(mLayering, C2_PARAMKEY_TEMPORAL_LAYERING)
			.withDefault(C2StreamTemporalLayeringTuning::output::AllocShared(0u, 0, 0, 0))
			.withFields({
				C2F(mLayering, m.layerCount).inRange(0, 4),
				C2F(mLayering, m.bLayerCount).inRange(0, 0),
				C2F(mLayering, m.bitrateRatios).inRange(0., 1.)
			})
			.withSetter(LayeringSetter)
			.build());

		addParameter(
			DefineParam(mPrependHeaderMode, C2_PARAMKEY_PREPEND_HEADER_MODE)
			.withDefault(new C2PrependHeaderModeSetting(PREPEND_HEADER_TO_NONE))
			.withFields({C2F(mPrependHeaderMode, value).any()})
			.withSetter(PrependHeaderModeSetter)
			.build());

	}

	static C2R SizeSetter(bool mayBlock,
			      const C2P<C2StreamPictureSizeInfo::input> &oldMe,
			      C2P<C2StreamPictureSizeInfo::input> &me)
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

	static C2R GopSetter(bool mayBlock, C2P<C2StreamGopTuning::output> &me)
	{
		(void)mayBlock;
		(void)me;
		return C2R::Ok();
	}

	static C2R RotationSetter(bool mayBlock, C2P<C2StreamRotationInfo::output> &me)
	{
		(void)mayBlock;
		// Note: SDK rotation is clock-wise, while C2 rotation is counter-clock-wise
		me.set().value = -(me.v.value);
		return C2R::Ok();
	}

	static C2R PictureQuantizationSetter(bool mayBlock,
					     C2P<C2StreamPictureQuantizationTuning::output> &me)
	{
		(void)mayBlock;
		(void)me;
		return C2R::Ok();
	}

	static C2R InputDelaySetter(
		bool mayBlock,
		C2P<C2PortActualDelayTuning::input> &me,
		const C2P<C2StreamGopTuning::output> &gop)
	{
		(void)mayBlock;
		uint32_t maxBframes = 0;
		ParseGop(gop.v, nullptr, nullptr, &maxBframes);
		me.set().value = maxBframes;
		return C2R::Ok();
	}

	static C2R BitrateSetter(bool mayBlock, C2P<C2StreamBitrateInfo::output> &me)
	{
		(void)mayBlock;
		C2R res = C2R::Ok();
		if (me.v.value <= 4096) {
		    me.set().value = 4096;
		}
		return res;
	}

	static C2R AVCProfileLevelSetter(
		bool mayBlock,
		C2P<C2StreamProfileLevelInfo::output> &me,
		const C2P<C2StreamPictureSizeInfo::input> &size,
		const C2P<C2StreamFrameRateInfo::output> &frameRate,
		const C2P<C2StreamBitrateInfo::output> &bitrate)
	{
		(void)mayBlock;
		if (!me.F(me.v.profile).supportsAtAll(me.v.profile)) {
			me.set().profile = PROFILE_AVC_MAIN;
		}

		struct LevelLimits {
			C2Config::level_t level;
			float mbsPerSec;
			uint64_t mbs;
			uint32_t bitrate;
		};
		constexpr LevelLimits kLimits[] = {
			{ LEVEL_AVC_1,     1485,    99,     64000 },
			// Decoder does not properly handle level 1b.
			// { LEVEL_AVC_1B,    1485,   99,   128000 },
			{ LEVEL_AVC_1_1,   3000,   396,    192000 },
			{ LEVEL_AVC_1_2,   6000,   396,    384000 },
			{ LEVEL_AVC_1_3,  11880,   396,    768000 },
			{ LEVEL_AVC_2,    11880,   396,   2000000 },
			{ LEVEL_AVC_2_1,  19800,   792,   4000000 },
			{ LEVEL_AVC_2_2,  20250,  1620,   4000000 },
			{ LEVEL_AVC_3,    40500,  1620,  10000000 },
			{ LEVEL_AVC_3_1, 108000,  3600,  14000000 },
			{ LEVEL_AVC_3_2, 216000,  5120,  20000000 },
			{ LEVEL_AVC_4,   245760,  8192,  20000000 },
			{ LEVEL_AVC_4_1, 245760,  8192,  50000000 },
			{ LEVEL_AVC_4_2, 522240,  8704,  50000000 },
			{ LEVEL_AVC_5,   589824, 22080, 135000000 },
		};

		uint64_t mbs = uint64_t((size.v.width + 15) / 16) * ((size.v.height + 15) / 16);
		float mbsPerSec = float(mbs) * frameRate.v.value;

		// Check if the supplied level meets the MB / bitrate requirements. If
		// not, update the level with the lowest level meeting the requirements.

		bool found = false;
		// By default needsUpdate = false in case the supplied level does meet
		// the requirements. For Level 1b, we want to update the level anyway,
		// so we set it to true in that case.
		bool needsUpdate = (me.v.level == LEVEL_AVC_1B);
		for (const LevelLimits &limit : kLimits) {
			if (mbs <= limit.mbs && mbsPerSec <= limit.mbsPerSec &&
				bitrate.v.value <= limit.bitrate) {
				// This is the lowest level that meets the requirements, and if
				// we haven't seen the supplied level yet, that means we don't
				// need the update.
				if (needsUpdate) {
					ALOGI("Given level %x does not cover current configuration: "
						"adjusting to %x", me.v.level, limit.level);
					me.set().level = limit.level;
				}
				found = true;
				break;
			}
			if (me.v.level == limit.level) {
				// We break out of the loop when the lowest feasible level is
				// found. The fact that we're here means that our level doesn't
				// meet the requirement and needs to be updated.
				needsUpdate = true;
			}
		}
		if (!found) {
			// We set to the highest supported level.
			me.set().level = LEVEL_AVC_5;
		}
	    
		return C2R::Ok();
	}

	static C2R HEVCProfileLevelSetter(
		bool mayBlock,
		C2P<C2StreamProfileLevelInfo::output> &me,
		const C2P<C2StreamPictureSizeInfo::input> &size,
		const C2P<C2StreamFrameRateInfo::output> &frameRate,
		const C2P<C2StreamBitrateInfo::output> &bitrate)
	{
		(void)mayBlock;
		if (!me.F(me.v.profile).supportsAtAll(me.v.profile)) {
		    me.set().profile = PROFILE_HEVC_MAIN;
		}

		struct LevelLimits {
			C2Config::level_t level;
			uint64_t samplesPerSec;
			uint64_t samples;
			uint32_t bitrate;
		};

		constexpr LevelLimits kLimits[] = {
			{ LEVEL_HEVC_MAIN_1,       552960,    36864,    128000 },
			{ LEVEL_HEVC_MAIN_2,      3686400,   122880,   1500000 },
			{ LEVEL_HEVC_MAIN_2_1,    7372800,   245760,   3000000 },
			{ LEVEL_HEVC_MAIN_3,     16588800,   552960,   6000000 },
			{ LEVEL_HEVC_MAIN_3_1,   33177600,   983040,  10000000 },
			{ LEVEL_HEVC_MAIN_4,     66846720,  2228224,  12000000 },
			{ LEVEL_HEVC_MAIN_4_1,  133693440,  2228224,  20000000 },
			{ LEVEL_HEVC_MAIN_5,    267386880,  8912896,  25000000 },
			{ LEVEL_HEVC_MAIN_5_1,  534773760,  8912896,  40000000 },
			{ LEVEL_HEVC_MAIN_5_2, 1069547520,  8912896,  60000000 },
			{ LEVEL_HEVC_MAIN_6,   1069547520, 35651584,  60000000 },
			{ LEVEL_HEVC_MAIN_6_1, 2139095040, 35651584, 120000000 },
			{ LEVEL_HEVC_MAIN_6_2, 4278190080, 35651584, 240000000 },
		};

		uint64_t samples = size.v.width * size.v.height;
		uint64_t samplesPerSec = samples * frameRate.v.value;

		// Check if the supplied level meets the MB / bitrate requirements. If
		// not, update the level with the lowest level meeting the requirements.

		bool found = false;
		// By default needsUpdate = false in case the supplied level does meet
		// the requirements.
		bool needsUpdate = false;
		for (const LevelLimits &limit : kLimits) {
			if (samples <= limit.samples && samplesPerSec <= limit.samplesPerSec &&
				bitrate.v.value <= limit.bitrate) {
				// This is the lowest level that meets the requirements, and if
				// we haven't seen the supplied level yet, that means we don't
				// need the update.
				if (needsUpdate) {
					ALOGI("Given level %x does not cover current configuration: "
						"adjusting to %x", me.v.level, limit.level);
					me.set().level = limit.level;
				}
				found = true;
				break;
			}
			if (me.v.level == limit.level) {
				// We break out of the loop when the lowest feasible level is
				// found. The fact that we're here means that our level doesn't
				// meet the requirement and needs to be updated.
				needsUpdate = true;
			}
		}
		if (!found) {
			// We set to the highest supported level.
			me.set().level = LEVEL_HEVC_MAIN_4_1;
		}
		return C2R::Ok();
	}

	static C2R DefaultProfileLevelSetter(
		bool mayBlock,
		C2P<C2StreamProfileLevelInfo::output> &me,
		const C2P<C2StreamPictureSizeInfo::input> &size,
		const C2P<C2StreamFrameRateInfo::output> &frameRate,
		const C2P<C2StreamBitrateInfo::output> &bitrate)
	{
		(void)mayBlock;
		(void)me;
		(void)size;
		(void)frameRate;
		(void)bitrate;
		return C2R::Ok();
	}

	static C2R ColorAspectsSetter(bool mayBlock, C2P<C2StreamColorAspectsInfo::input> &me)
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

	static C2R CodedColorAspectsSetter(bool mayBlock, C2P<C2StreamColorAspectsInfo::output> &me,
					   const C2P<C2StreamColorAspectsInfo::input> &coded)
	{
		(void)mayBlock;
		me.set().range = coded.v.range;
		me.set().primaries = coded.v.primaries;
		me.set().transfer = coded.v.transfer;
		me.set().matrix = coded.v.matrix;
		return C2R::Ok();
	}

	static C2R LayeringSetter(
		bool mayBlock, C2P<C2StreamTemporalLayeringTuning::output>& me)
	{
		(void)mayBlock;
		(void)me;
		return C2R::Ok();
	}

	static C2R PrependHeaderModeSetter(
		bool mayBlock, C2P<C2PrependHeaderModeSetting>& me)
	{
		(void)mayBlock;
		(void)me;
		return C2R::Ok();
	}

	uint32_t getSyncFramePeriod_l() const
	{
		if (mSyncFramePeriod->value < 0 || mSyncFramePeriod->value == INT64_MAX) {
			return 0;
		}
		double period = mSyncFramePeriod->value / 1e6 * mFrameRate->value;
		return (uint32_t)c2_max(c2_min(period + 0.5, double(UINT32_MAX)), 1.);
	}

	uint32_t getBitrateMode_l() const
	{
		int32_t cMode = mBitrateMode->value;
		
		return getMppBitrateMode(cMode, true);
	}

	// unsafe getters
	std::shared_ptr<C2StreamPictureSizeInfo::input> getSize_l() const
	{ return mSize; }
	std::shared_ptr<C2StreamIntraRefreshTuning::output> getIntraRefresh_l() const
	{ return mIntraRefresh; }
	std::shared_ptr<C2StreamFrameRateInfo::output> getFrameRate_l() const
	{ return mFrameRate; }
	std::shared_ptr<C2StreamBitrateInfo::output> getBitrate_l() const
	{ return mBitrate; }
	std::shared_ptr<C2StreamRequestSyncFrameTuning::output> getRequestSync_l() const
	{ return mRequestSync; }
	std::shared_ptr<C2StreamGopTuning::output> getGop_l() const
	{ return mGop; }
	std::shared_ptr<C2StreamRotationInfo::output> getRotation_l() const
	{ return mRotation; }
	std::shared_ptr<C2StreamPictureQuantizationTuning::output> getPictureQuantization_l() const
	{ return mPictureQuantization; }
	std::shared_ptr<C2StreamColorAspectsInfo::output> getCodedColorAspects_l() const
	{ return mCodedColorAspects; }
	std::shared_ptr<C2StreamTemporalLayeringTuning::output> getTemporalLayers_l() const
	{ return mLayering; }
	std::shared_ptr<C2PrependHeaderModeSetting> getPrependHeaderMode_l() const
	{ return mPrependHeaderMode; }

private:
	std::shared_ptr<C2StreamUsageTuning::input> mUsage;
	std::shared_ptr<C2StreamPictureSizeInfo::input> mSize;
	std::shared_ptr<C2StreamFrameRateInfo::output> mFrameRate;
	std::shared_ptr<C2StreamRequestSyncFrameTuning::output> mRequestSync;
	std::shared_ptr<C2StreamIntraRefreshTuning::output> mIntraRefresh;
	std::shared_ptr<C2StreamBitrateInfo::output> mBitrate;
	std::shared_ptr<C2StreamProfileLevelInfo::output> mProfileLevel;
	std::shared_ptr<C2StreamSyncFrameIntervalTuning::output> mSyncFramePeriod;
	std::shared_ptr<C2StreamGopTuning::output> mGop;
	std::shared_ptr<C2StreamRotationInfo::output> mRotation;
	std::shared_ptr<C2StreamPictureQuantizationTuning::output> mPictureQuantization;
	std::shared_ptr<C2StreamBitrateModeTuning::output> mBitrateMode;
	std::shared_ptr<C2StreamColorAspectsInfo::input> mColorAspects;
	std::shared_ptr<C2StreamColorAspectsInfo::output> mCodedColorAspects;
	std::shared_ptr<C2StreamTemporalLayeringTuning::output> mLayering;
	std::shared_ptr<C2PrependHeaderModeSetting> mPrependHeaderMode;
};

} // namespace codec2::rkmpp

#endif // C2_RK_MPP_ENCODER_HPP
