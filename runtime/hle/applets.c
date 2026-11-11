// System screens the game can open (library applets): the Joy-Con strap guide, error dialogs, the web browser and the
// eShop. On the Switch each one is launched through the applet manager and the game waits until the player closes it;
// the runtime has no library applets (a launch would wait forever), so each returns at once, as if the player had
// closed it straight away. Error dialogs are logged. (The software keyboard is runtime/hle/swkbd.c.)
#include "../hle.h"
#include "../ui/menu.h"

// void nn::hid::ShowControllerStrapGuide()
HLE_FUNC("_ZN2nn3hid24ShowControllerStrapGuideEv", applet_strap_guide) {
    LOG("[applet] controller strap guide (skipped)\n");
    c->x[0] = 0;
}

// void nn::err::ShowError(nn::err::ErrorCode)  (ErrorCode {u32 category, u32 number} in x0)
HLE_FUNC("_ZN2nn3err9ShowErrorENS0_9ErrorCodeE", applet_show_error_code) {
    LOG("[applet] error dialog: %04u-%04u (skipped)\n", (unsigned)(c->x[0] & 0xFFFFFFFF), (unsigned)(c->x[0] >> 32));
}

// void nn::err::ShowError(nn::Result)
HLE_FUNC("_ZN2nn3err9ShowErrorENS_6ResultE", applet_show_error_result) {
    uint32_t r = (uint32_t)c->x[0];
    LOG("[applet] error dialog: result %#x (%04u-%04u) (skipped)\n", r, 2000 + (r & 0x1FF), (r >> 9) & 0x1FFF);
}

// void nn::err::ShowApplicationError(const nn::err::ApplicationErrorArg&)
HLE_FUNC("_ZN2nn3err20ShowApplicationErrorERKNS0_19ApplicationErrorArgE", applet_show_app_error) {
    (void)c;
    LOG("[applet] application error dialog (skipped)\n");
}

// nn::Result nn::web::ShowWebPage(nn::web::WebPageReturnValue*, const nn::web::ShowWebPageArg&)
HLE_FUNC("_ZN2nn3web11ShowWebPageEPNS0_18WebPageReturnValueERKNS0_14ShowWebPageArgE", applet_web_page) {
    LOG("[applet] web page (skipped)\n");
    menu_notice("The game's web pages (the online manual) aren't available in ARMS Rearmed.", 5);
    c->x[0] = 0;  // (the return value as the game constructed it: closed)
}

// void nn::ec::ShowShop...(const nn::account::UserHandle&): the eShop
HLE_FUNC("_ZN2nn2ec22ShowShopEnterCodeSceneERKNS_7account10UserHandleE", applet_shop_code) {
    (void)c;
    LOG("[applet] eShop: enter code (skipped)\n");
    menu_notice("The Nintendo eShop isn't available in ARMS Rearmed.", 5);
}
HLE_FUNC("_ZN2nn2ec24ShowShopAddOnContentListERKNS_7account10UserHandleE", applet_shop_aoc) {
    (void)c;
    LOG("[applet] eShop: add-on content (skipped)\n");
    menu_notice("The Nintendo eShop isn't available in ARMS Rearmed.", 5);
}
HLE_FUNC("_ZN2nn2ec26ShowShopConsumableItemListERKNS_7account10UserHandleE", applet_shop_items) {
    (void)c;
    LOG("[applet] eShop: consumable items (skipped)\n");
    menu_notice("The Nintendo eShop isn't available in ARMS Rearmed.", 5);
}
