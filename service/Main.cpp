//#define LOG_NDEBUG 0
#define LOG_TAG "android.hardware.media.c2-rkmpp-service"

#include <android-base/logging.h>
#include <minijail.h>

#include <util/C2InterfaceHelper.h>
#include <C2Component.h>
#include <C2Config.h>

#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <codec2/aidl/ComponentStore.h>
#include <codec2/aidl/ParamTypes.h>

#include <C2RkMppComponentStore.hpp>

using namespace aidl::android::hardware::media::c2;
using namespace codec2::rkmpp;

// This is the absolute on-device path of the prebuild_etc module
// "android.hardware.media.c2-default-seccomp_policy" in Android.bp.
static constexpr char kBaseSeccompPolicyPath[] =
	"/vendor/etc/seccomp_policy/"
	"android.hardware.media.c2-rkmpp-service-seccomp_policy";

// Additional seccomp permissions can be added in this file.
// This file does not exist by default.
static constexpr char kExtSeccompPolicyPath[] =
	"/vendor/etc/seccomp_policy/"
	"android.hardware.media.c2-extended-seccomp_policy";

// We want multiple threads to be running so that a blocking operation
// on one codec does not block the other codecs.
static constexpr int kThreadCount = 8;

int main(int /* argc */, char** /* argv */)
{
	LOG(DEBUG) << "android.hardware.media.c2-rkmpp-service starting...";

	// TODO: Enable minijail
	// Set up minijail to limit system calls.
	//signal(SIGPIPE, SIG_IGN);
	//android::SetUpMinijail(kBaseSeccompPolicyPath, kExtSeccompPolicyPath);

	ABinderProcess_setThreadPoolMaxThreadCount(kThreadCount);
	ABinderProcess_startThreadPool();

	// Create IComponentStore service.
	using namespace ::aidl::android::hardware::media::c2;
	std::shared_ptr<IComponentStore> store;

	LOG(DEBUG) << "Instantiating Codec2's IComponentStore service...";
	store = ::ndk::SharedRefBase::make<utils::ComponentStore>(
		std::make_shared<C2RkMppComponentStore>());

	if (store == nullptr) {
		LOG(ERROR) << "Cannot create Codec2's IComponentStore service.";
	} else {
		const std::string serviceName = std::string(IComponentStore::descriptor) + "/default";
		binder_exception_t ex = AServiceManager_addService(
			store->asBinder().get(), serviceName.c_str());
		if (ex != EX_NONE) {
			LOG(ERROR) << "Cannot register Codec2's IComponentStore service"
				      " with instance name << \""
				      << serviceName << "\".";
		} else {
			LOG(DEBUG) << "Codec2's IComponentStore service registered. "
				      "Instance name: \"" << serviceName << "\".";
		}
	}

	ABinderProcess_joinThreadPool();

	return 0;
}
