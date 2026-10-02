/* stub_recboot.c - stub when boot recording is compiled out */

#include "capture_rec.h"

void cap_session_arm(void) { }

void cap_session_frame(const UINT32 *pixels, UINTN w, UINTN h) {
    (void)pixels; (void)w; (void)h;
}

void cap_session_finish(void) { }

int cap_session_active(void) { return 0; }
