// The data folder chosen on iOS: the system's document picker for a folder.
//
// SDL has no folder dialog on iOS, and an iOS app may only open a folder
// outside its own container through the security-scoped URL the document
// picker hands it - for this run - and through a bookmark of that URL on
// later runs. So the picker's answer is the path plus the bookmark
// (DataFolderPicker::deliver), and startup resolves the bookmark again
// (iosResolveFolderBookmark, main.cpp selectUserDataPath).
//
// Compiled with ARC (CMakeLists.txt).

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <SDL3/SDL.h>

#include <string>

#include "ui/data_folder_picker.hpp"

@interface WoweeFolderPickerDelegate : NSObject <UIDocumentPickerDelegate>
@end

// The picker holds its delegate weakly; this keeps it alive until it answers.
static WoweeFolderPickerDelegate* g_folderPickerDelegate = nil;

@implementation WoweeFolderPickerDelegate

- (void)documentPicker:(UIDocumentPickerViewController*)controller
    didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    (void)controller;
    NSURL* url = urls.firstObject;
    std::string path, bookmark, error;
    if (url != nil) {
        // Open for this run; the bookmark opens it on later ones.
        [url startAccessingSecurityScopedResource];
        NSError* err = nil;
        NSData* data = [url bookmarkDataWithOptions:0
                     includingResourceValuesForKeys:nil
                                      relativeToURL:nil
                                              error:&err];
        if (data != nil) {
            bookmark = [[data base64EncodedStringWithOptions:0] UTF8String];
        }
        path = url.path.UTF8String ? url.path.UTF8String : "";
        if (path.empty()) error = "the chosen folder has no path";
    }
    wowee::ui::dataFolderPicker().deliver(path, bookmark, error);
    g_folderPickerDelegate = nil;
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
    (void)controller;
    wowee::ui::dataFolderPicker().deliver("", "", "");
    g_folderPickerDelegate = nil;
}

@end

namespace wowee::ui {

bool iosPresentFolderPicker(SDL_Window* window) {
    if (window == nullptr) return false;
    UIWindow* uiWindow = (__bridge UIWindow*)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    UIViewController* root = uiWindow.rootViewController;
    if (root == nil) return false;
    // Over whatever is already presented, or the presentation is refused.
    while (root.presentedViewController != nil) root = root.presentedViewController;

    UIDocumentPickerViewController* picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeFolder ]];
    g_folderPickerDelegate = [WoweeFolderPickerDelegate new];
    picker.delegate = g_folderPickerDelegate;
    picker.allowsMultipleSelection = NO;
    [root presentViewController:picker animated:YES completion:nil];
    return true;
}

std::string iosResolveFolderBookmark(const std::string& base64) {
    NSString* text = [NSString stringWithUTF8String:base64.c_str()];
    if (text == nil) return {};
    NSData* data = [[NSData alloc] initWithBase64EncodedString:text options:0];
    if (data == nil) return {};
    BOOL stale = NO;
    NSError* err = nil;
    NSURL* url = [NSURL URLByResolvingBookmarkData:data
                                           options:0
                                     relativeToURL:nil
                               bookmarkDataIsStale:&stale
                                             error:&err];
    if (url == nil) return {};
    if (![url startAccessingSecurityScopedResource]) return {};
    const char* path = url.path.UTF8String;
    return path ? std::string(path) : std::string();
}

}  // namespace wowee::ui
