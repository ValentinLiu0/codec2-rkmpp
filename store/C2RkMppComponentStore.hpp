#ifndef C2_RK_MPP_COMPONENT_STORE_HPP
#define C2_RK_MPP_COMPONENT_STORE_HPP

#include <memory>
#include <vector>

#include <codec2/aidl/ComponentStore.h>
#include <codec2/aidl/ParamTypes.h>
#include <media/stagefright/foundation/MediaDefs.h>
#include <util/C2InterfaceHelper.h>

namespace codec2::rkmpp {

struct C2RkMppComponentEntry {
	C2String		name;
	C2String		mime;
	C2Component::kind_t	kind;
};

static C2RkMppComponentEntry sAllComponentsMap[] = {
	/* Hardware decoder list */
	{ "c2.rkmpp.h263.decoder",	::android::MEDIA_MIMETYPE_VIDEO_H263,	C2Component::KIND_DECODER },
	{ "c2.rkmpp.avc.decoder",	::android::MEDIA_MIMETYPE_VIDEO_AVC,	C2Component::KIND_DECODER },
	{ "c2.rkmpp.hevc.decoder",	::android::MEDIA_MIMETYPE_VIDEO_HEVC,	C2Component::KIND_DECODER },
	{ "c2.rkmpp.vp8.decoder",	::android::MEDIA_MIMETYPE_VIDEO_VP8,	C2Component::KIND_DECODER },
	{ "c2.rkmpp.vp9.decoder",	::android::MEDIA_MIMETYPE_VIDEO_VP9,	C2Component::KIND_DECODER },
	{ "c2.rkmpp.mpeg2.decoder",	::android::MEDIA_MIMETYPE_VIDEO_MPEG2,	C2Component::KIND_DECODER },
	{ "c2.rkmpp.m4v.decoder",	::android::MEDIA_MIMETYPE_VIDEO_MPEG4,	C2Component::KIND_DECODER },
	{ "c2.rkmpp.av1.decoder",	::android::MEDIA_MIMETYPE_VIDEO_AV1,	C2Component::KIND_DECODER },
	/* Hardware encoder list */
	{ "c2.rkmpp.avc.encoder",	::android::MEDIA_MIMETYPE_VIDEO_AVC,	C2Component::KIND_ENCODER },
	{ "c2.rkmpp.hevc.encoder",	::android::MEDIA_MIMETYPE_VIDEO_HEVC,	C2Component::KIND_ENCODER },
	{ "c2.rkmpp.vp8.encoder",	::android::MEDIA_MIMETYPE_VIDEO_VP8,	C2Component::KIND_ENCODER },
};
static size_t sAllComponentsMapSize = sizeof(sAllComponentsMap) / sizeof(sAllComponentsMap[0]);

class C2RkMppComponentStore : public C2ComponentStore {
public:
	C2RkMppComponentStore();
	virtual ~C2RkMppComponentStore() override = default;

	C2String getName() const override;
	c2_status_t createComponent(C2String name, std::shared_ptr<C2Component> *const component) override;
	c2_status_t createInterface(C2String name, std::shared_ptr<C2ComponentInterface> *const interface) override;
	std::vector<std::shared_ptr<const C2Component::Traits>> listComponents() override;
	c2_status_t copyBuffer(std::shared_ptr<C2GraphicBuffer> src, std::shared_ptr<C2GraphicBuffer> dst) override;
	c2_status_t query_sm(const std::vector<C2Param*> &stackParams,
			     const std::vector<C2Param::Index> &heapParamIndices,
			     std::vector<std::unique_ptr<C2Param>> *const heapParams) const override;
	c2_status_t config_sm(const std::vector<C2Param*> &params,
			      std::vector<std::unique_ptr<C2SettingResult>> *const failures) override;
	std::shared_ptr<C2ParamReflector> getParamReflector() const override;
	c2_status_t querySupportedParams_nb(std::vector<std::shared_ptr<C2ParamDescriptor>> *const params) const override;
	c2_status_t querySupportedValues_sm(std::vector<C2FieldSupportedValuesQuery> &fields) const override;

private:
	// The AIDL ComponentStore wrapper dereferences getParamReflector()'s
	// result unconditionally, so this must never be null.
	std::shared_ptr<C2ReflectorHelper> mReflector;
	std::vector<C2RkMppComponentEntry> mComponents = {};

	/**
	 * Check is the component supported by hardware by calling libmpp functions.
	 */
	bool isHardwareSupported(C2String name);
};

std::shared_ptr<C2ComponentStore> GetC2RkMppComponentStore();

} // namespace codec2::rkmpp

#endif /* C2_RK_MPP_COMPONENT_STORE_HPP */
