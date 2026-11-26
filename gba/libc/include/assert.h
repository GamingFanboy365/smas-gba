#pragma once
// Asserts are off in the GBA build unless SMAS_ASSERTS is defined; then a
// failing one is logged (with mGBA's debug log) and the game carries on.
#ifdef SMAS_ASSERTS
void GbaAssertFailed(const char *expr, const char *file, int line);
#define assert(e) ((e) ? (void)0 : GbaAssertFailed(#e, __FILE__, __LINE__))
#else
#define assert(e) ((void)0)
#endif
