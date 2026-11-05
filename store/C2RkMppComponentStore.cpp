//#define LOG_NDEBUG 0
#define LOG_TAG "C2RkMppComponentStore"
#include <log/log.h>

#include <strings.h>

#include <mpp/osal/mpp_soc.h>
#include <mpp/rk_mpi.h>

#include <C2RkMppComponentInterface.hpp>
#include <C2RkMppComponentStore.hpp>
#include <C2RkMppDecoder.hpp>
#include <C2RkMppEncoder.hpp>
#include <C2RkMppUtils.hpp>

namespace codec2::rkmpp {

namespace {

// Rank the hardware components ahead of the Codec2 software ones (c2.android.*
// use 0x200), matching the rank OMX hardware components traditionally use.
constexpr C2Component::rank_t kHardwareRank = 0x100;

} // namespace

int32_t GetMppCodingFromComponentName(C2String name)
{
	for (int i = 0; i < sAllComponentsMapSize; ++i) {
		if (!strcasecmp(name.c_str(), sAllComponentsMap[i].name.c_str())) {
			return mppCodingFromMime(sAllComponentsMap[i].mime);
		}
	}
	return MPP_VIDEO_CodingUnused;
}

int32_t GetMppCtxTypeFromComponentName(C2String name)
{
	for (int i = 0; i < sAllComponentsMapSize; ++i) {
		if (!strcasecmp(name.c_str(), sAllComponentsMap[i].name.c_str())) {
			if (sAllComponentsMap[i].kind == C2Component::KIND_DECODER) {
				return MPP_CTX_DEC;
			} else if (sAllComponentsMap[i].kind == C2Component::KIND_ENCODER) {
				return MPP_CTX_ENC;
			}
		}
	}
	return -1;
}

C2RkMppComponentEntry* GetRkMppComponentEntry(C2String name)
{
	for (int i = 0; i < sAllComponentsMapSize; ++i) {
		if (!strcasecmp(name.c_str(), sAllComponentsMap[i].name.c_str())) {
			return &sAllComponentsMap[i];
		}
	}
	return nullptr;
}

bool C2RkMppComponentStore::isHardwareSupported(C2String name)
{
	int32_t coding = GetMppCodingFromComponentName(name);
	int32_t type = GetMppCtxTypeFromComponentName(name);

	if (!mpp_check_soc_cap((MppCtxType)type, (MppCodingType)coding)) {
		return false;
	}
	return true;
}

C2RkMppComponentStore::C2RkMppComponentStore()
	: mReflector(std::make_shared<C2ReflectorHelper>())
{
	for (int i = 0; i < sAllComponentsMapSize; ++i) {
		if (isHardwareSupported(sAllComponentsMap[i].name)) {
			mComponents.push_back(sAllComponentsMap[i]);
		}
	}
	ALOGD("Initialized C2Store with hardware supported components:");
	for (int i = 0; i < mComponents.size(); ++i) {
		ALOGD("\t%s", mComponents[i].name.c_str());
	}
}

C2String C2RkMppComponentStore::getName() const
{
	return "android.componentStore.rkmpp";
}

namespace {
std::atomic<c2_node_id_t> gNextNodeId{1};
}

c2_status_t C2RkMppComponentStore::createComponent(C2String name, std::shared_ptr<C2Component> *const component)
{
	component->reset();
	C2RkMppComponentEntry *entry = GetRkMppComponentEntry(name);
	if (entry == nullptr) {
		return C2_BAD_VALUE;
	}
	c2_node_id_t id = gNextNodeId++;

	if (entry->kind == C2Component::KIND_DECODER) {
		auto intfImpl = std::make_shared<C2RkMppDecoder::IntfImpl>(
				mReflector, name, entry->kind, C2Component::DOMAIN_VIDEO, entry->mime);
		*component = std::make_shared<C2RkMppDecoder>(entry->name.c_str(), entry->mime.c_str(), id, intfImpl);
	} else if (entry->kind == C2Component::KIND_ENCODER) {
		auto intfImpl = std::make_shared<C2RkMppEncoder::IntfImpl>(
				mReflector, name, entry->kind, C2Component::DOMAIN_VIDEO, entry->mime);
		*component = std::make_shared<C2RkMppEncoder>(entry->name.c_str(), entry->mime.c_str(), id, intfImpl);
	} else {
		return C2_BAD_VALUE;
	}
	return C2_OK;
}

c2_status_t C2RkMppComponentStore::createInterface(C2String name, std::shared_ptr<C2ComponentInterface> *const interface)
{
	interface->reset();
	C2RkMppComponentEntry *entry = GetRkMppComponentEntry(name);
	if (entry == nullptr) {
		return C2_BAD_VALUE;
	}
	c2_node_id_t id = gNextNodeId++;

	if (entry->kind == C2Component::KIND_DECODER) {
		auto intfImpl = std::make_shared<C2RkMppDecoder::IntfImpl>(
				mReflector, name, entry->kind, C2Component::DOMAIN_VIDEO, entry->mime);
		*interface = std::make_shared<C2RkMppComponentInterface<C2RkMppDecoder::IntfImpl>>(
				name.c_str(), id, intfImpl);
	} else if (entry->kind == C2Component::KIND_ENCODER) {
		auto intfImpl = std::make_shared<C2RkMppEncoder::IntfImpl>(
				mReflector, name, entry->kind, C2Component::DOMAIN_VIDEO, entry->mime);
		*interface = std::make_shared<C2RkMppComponentInterface<C2RkMppEncoder::IntfImpl>>(
				name.c_str(), id, intfImpl);
	} else {
		return C2_BAD_VALUE;
	}
	return C2_OK;
}

std::vector<std::shared_ptr<const C2Component::Traits>>
C2RkMppComponentStore::listComponents()
{
	std::vector<std::shared_ptr<const C2Component::Traits>> traits;
	for (const C2RkMppComponentEntry &entry : mComponents) {
		std::shared_ptr<C2Component::Traits> trait = std::make_shared<C2Component::Traits>();
		trait->name = entry.name;
		trait->domain = C2Component::DOMAIN_VIDEO;
		trait->kind = entry.kind;
		trait->rank = kHardwareRank;
		trait->mediaType = entry.mime;
		trait->owner = getName();
		traits.push_back(std::move(trait));
	}
	ALOGV("listComponents(): %zu component(s)", traits.size());
	return traits;
}

c2_status_t C2RkMppComponentStore::copyBuffer(std::shared_ptr<C2GraphicBuffer> src, std::shared_ptr<C2GraphicBuffer> dst)
{
	// No swizzling conversion is provided; the framework falls back to its own.
	(void)src;
	(void)dst;
	return C2_OMITTED;
}

c2_status_t C2RkMppComponentStore::query_sm(const std::vector<C2Param*> &stackParams,
					    const std::vector<C2Param::Index> &heapParamIndices,
					    std::vector<std::unique_ptr<C2Param>> *const heapParams) const
{
	(void)heapParams;
	for (C2Param *param : stackParams) {
		param->invalidate();
	}
	return (stackParams.empty() && heapParamIndices.empty()) ? C2_OK : C2_BAD_INDEX;
}

c2_status_t C2RkMppComponentStore::config_sm(const std::vector<C2Param*> &params,
					     std::vector<std::unique_ptr<C2SettingResult>> *const failures)
{
	(void)failures;
	return params.empty() ? C2_OK : C2_BAD_INDEX;
}

std::shared_ptr<C2ParamReflector> C2RkMppComponentStore::getParamReflector() const
{
	return mReflector;
}

c2_status_t C2RkMppComponentStore::querySupportedParams_nb(std::vector<std::shared_ptr<C2ParamDescriptor>> *const params) const
{
	// The store does not expose any system-wide parameters yet.
	(void)params;
	return C2_OK;
}

c2_status_t C2RkMppComponentStore::querySupportedValues_sm(std::vector<C2FieldSupportedValuesQuery> &fields) const
{
	// No store-level field is supported, so none can be answered.
	for (C2FieldSupportedValuesQuery &field : fields) {
		field.status = C2_BAD_INDEX;
	}
	return fields.empty() ? C2_OK : C2_BAD_INDEX;
}

std::shared_ptr<C2ComponentStore> GetC2RkMppComponentStore()
{
	static std::mutex mutex;
	static std::weak_ptr<C2ComponentStore> platformStore;
	std::lock_guard<std::mutex> lock(mutex);
	std::shared_ptr<C2ComponentStore> store = platformStore.lock();
	if (store == nullptr) {
		store = std::make_shared<C2RkMppComponentStore>();
		platformStore = store;
	}
	return store;
}

} // namespace codec2::rkmpp
