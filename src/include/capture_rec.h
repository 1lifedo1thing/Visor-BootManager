/* capture_rec.h - whole-boot session recording (feature: recboot) */
#ifndef VISOR_CAPTURE_REC_H
#define VISOR_CAPTURE_REC_H

#include <efi.h>

/* Looks for the arming marker on the ESP and, if present, deletes it and puts
 * the recorder in business. Call before anything is drawn - the recording is
 * meant to start at the first pixel of the fade-in. */
void cap_session_arm(void);

/* Offer the current screen to the recorder. Cheap and safe to call on every
 * present: frames are taken on a wall-clock schedule, everything else returns
 * immediately. */
void cap_session_frame(const UINT32 *pixels, UINTN w, UINTN h);

/* Finish the file. Idempotent, and must happen before ExitBootServices. */
void cap_session_finish(void);

int cap_session_active(void);

#endif
