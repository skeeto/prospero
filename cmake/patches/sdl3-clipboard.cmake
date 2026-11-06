# SDL 3.4.18's Cocoa clipboard read the pasteboard's types twice, to
# count them and then to copy them, and a pasteboard changed between the
# two left slots null, which SDL_SaveClipboardMimeTypes then strdup'ed:
# Prospero crashed as its window became key while a dictation app had
# just pasted and restored the clipboard. This replaces GetMimeTypes with
# one that reads them once. It applies to a downloaded SDL (PATCH_COMMAND)
# and to a bundled deps/sdl3 alike, needs neither git nor patch, and does
# nothing to a file patched already; a file of another SDL, where the
# function is not found as it was, fails the configure.
#
#   cmake -DSRC=<SDL source dir> -P sdl3-clipboard.cmake

set(file "${SRC}/src/video/cocoa/SDL_cocoaclipboard.m")
file(READ "${file}" text)
if(text MATCHES "PROSPERO_PATCHED")
  return()
endif()
set(old [==[static char **GetMimeTypes(int *pnformats)
{
    char **new_mime_types = NULL;

    *pnformats = 0;

    int nformats = 0;
    int formatsSz = 0;
    NSArray<NSPasteboardItem *> *items = [[NSPasteboard generalPasteboard] pasteboardItems];
    NSUInteger nitems = [items count];
    if (nitems > 0) {
        for (NSPasteboardItem *item in items) {
            NSArray<NSString *> *types = [item types];
            for (NSString *type in types) {
                if (@available(macOS 11.0, *)) {
                    UTType *uttype = [UTType typeWithIdentifier:type];
                    NSString *mime_type = [uttype preferredMIMEType];
                    if (mime_type) {
                        NSUInteger len = [mime_type lengthOfBytesUsingEncoding:NSUTF8StringEncoding] + 1;
                        formatsSz += len;
                        ++nformats;
                    }
                }
                NSUInteger len = [type lengthOfBytesUsingEncoding:NSUTF8StringEncoding] + 1;
                formatsSz += len;
                ++nformats;
            }
        }

        new_mime_types = SDL_AllocateTemporaryMemory((nformats + 1) * sizeof(char *) + formatsSz);
        if (new_mime_types) {
            int i = 0;
            char *strPtr = (char *)(new_mime_types + nformats + 1);
            for (NSPasteboardItem *item in items) {
                NSArray<NSString *> *types = [item types];
                for (NSString *type in types) {
                    if (@available(macOS 11.0, *)) {
                        UTType *uttype = [UTType typeWithIdentifier:type];
                        NSString *mime_type = [uttype preferredMIMEType];
                        if (mime_type) {
                            NSUInteger len = [mime_type lengthOfBytesUsingEncoding:NSUTF8StringEncoding] + 1;
                            SDL_memcpy(strPtr, [mime_type UTF8String], len);
                            new_mime_types[i++] = strPtr;
                            strPtr += len;
                        }
                    }
                    NSUInteger len = [type lengthOfBytesUsingEncoding:NSUTF8StringEncoding] + 1;
                    SDL_memcpy(strPtr, [type UTF8String], len);
                    new_mime_types[i++] = strPtr;
                    strPtr += len;
                }
            }

            new_mime_types[nformats] = NULL;
            *pnformats = nformats;
        }
    }
    return new_mime_types;
}


]==])
set(new [==[static char **GetMimeTypes(int *pnformats)
{
    char **new_mime_types = NULL;

    *pnformats = 0;

    // PROSPERO_PATCHED: the pasteboard's types are read once, into names,
    // and both passes use that, as another process may change the
    // pasteboard between them (a dictation app pastes, then restores the
    // clipboard), which left slots null, or overran the allocation.
    NSArray<NSPasteboardItem *> *items = [[NSPasteboard generalPasteboard] pasteboardItems];
    if ([items count] == 0) {
        return NULL;
    }
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    for (NSPasteboardItem *item in items) {
        for (NSString *type in [item types]) {
            if (@available(macOS 11.0, *)) {
                UTType *uttype = [UTType typeWithIdentifier:type];
                NSString *mime_type = [uttype preferredMIMEType];
                if (mime_type) {
                    [names addObject:mime_type];
                }
            }
            [names addObject:type];
        }
    }

    int nformats = (int)[names count];
    size_t formatsSz = 0;
    for (NSString *name in names) {
        const char *s = [name UTF8String];
        formatsSz += s ? SDL_strlen(s) + 1 : 0;
    }
    new_mime_types = SDL_AllocateTemporaryMemory((nformats + 1) * sizeof(char *) + formatsSz);
    if (new_mime_types) {
        int i = 0;
        char *strPtr = (char *)(new_mime_types + nformats + 1);
        char *strEnd = strPtr + formatsSz;
        for (NSString *name in names) {
            const char *s = [name UTF8String];
            size_t len = s ? SDL_strlen(s) + 1 : 0;
            if (!s || i >= nformats || len > (size_t)(strEnd - strPtr)) {
                continue;
            }
            SDL_memcpy(strPtr, s, len);
            new_mime_types[i++] = strPtr;
            strPtr += len;
        }
        new_mime_types[i] = NULL;
        *pnformats = i;
    }
    return new_mime_types;
}


]==])
string(FIND "${text}" "${old}" at)
if(at EQUAL -1)
  message(FATAL_ERROR "${file}: GetMimeTypes is not as in SDL 3.4.18: update cmake/patches/sdl3-clipboard.cmake")
endif()
string(REPLACE "${old}" "${new}" text "${text}")
file(WRITE "${file}" "${text}")
message(STATUS "SDL3: patched GetMimeTypes (cmake/patches/sdl3-clipboard.cmake)")
