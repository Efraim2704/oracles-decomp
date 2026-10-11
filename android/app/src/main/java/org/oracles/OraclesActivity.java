package org.oracles;

import android.view.HapticFeedbackConstants;
import android.view.View;
import org.libsdl.app.SDLActivity;

public class OraclesActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }

    // A key tap's feedback on the game's view (called from C, see src/platform/haptic.c). It follows
    // the system's touch feedback setting, and runs on the UI thread as views expect.
    public static void haptic() {
        final View view = mSurface;
        if (view == null) return;
        view.post(new Runnable() {
            @Override
            public void run() {
                view.performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
            }
        });
    }
}
