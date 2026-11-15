#pragma once

#if !defined(TWICE_ABI) || TWICE_ABI != 2
#error "TWICE_ABI must be exported by the cmath target (public_defines)"
#endif

#ifdef __cplusplus
extern "C" {
#endif

int twice(int value);

#ifdef __cplusplus
}
#endif
