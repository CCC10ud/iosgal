/* stub/malloc.h — POSIX/macOS 拦截头
 * macOS SDK 没有 <malloc.h>；官方 TJS2 的 tjsCommHead.h 与 bison 生成的
 * *.tab.cpp 会 #include <malloc.h>。构建脚本把本目录放在 -I 最前，
 * 让这些 include 命中本空头。malloc/free/realloc 由 <stdlib.h> 提供
 * （tjs2_compat.h 已 #include <stdlib.h>）。
 */
#ifndef GALBOX_STUB_MALLOC_H
#define GALBOX_STUB_MALLOC_H
#include <stdlib.h>
#endif
