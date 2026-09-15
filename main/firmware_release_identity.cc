#include "firmware_release_identity.h"
#include "sdkconfig.h"

#if CONFIG_AUDREAM_WATCH_UI
static_assert(sizeof(BOARD_TYPE) <= sizeof(firmware_release::ImageIdentity::model));
#if defined(__APPLE__)
#define AUDREAM_RELEASE_SECTION "__DATA,__audream_ota"
#else
#define AUDREAM_RELEASE_SECTION ".rodata_custom_desc"
#endif
// ESP-IDF places custom application metadata directly after esp_app_desc_t.
extern "C" const firmware_release::ImageIdentity audream_release_identity
    __attribute__((used, section(AUDREAM_RELEASE_SECTION), aligned(4))) = {
        "AUDOTA1", CONFIG_AUDREAM_OTA_RELEASE_SEQUENCE, BOARD_TYPE
    };
#endif

const firmware_release::ImageIdentity* firmware_release::CurrentIdentity() {
#if CONFIG_AUDREAM_WATCH_UI
    return &audream_release_identity;
#else
    return nullptr;
#endif
}
