/*
 * Copyright (C) 2023 The PixelOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package org.pixelexperience.xiaomi.voipfix;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

public class BootCompletedReceiver extends BroadcastReceiver {
    private static final String TAG = "XiaomiVoIPFix";
    private static final boolean DEBUG = false;

    private static final String[] VALID_ACTIONS = {
            Intent.ACTION_BOOT_COMPLETED,
            "android.intent.action.LOCKED_BOOT_COMPLETED"
    };

    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null || intent.getAction() == null) {
            Log.w(TAG, "Received null intent or action, ignoring");
            return;
        }

        if (!isValidBootAction(intent.getAction())) {
            Log.w(TAG, "Unexpected action: " + intent.getAction() + ", ignoring");
            return;
        }

        if (DEBUG)
            Log.d(TAG, "Starting VoIP Fix service on boot");
        context.startService(new Intent(context, VoIPFixService.class));
    }

    private boolean isValidBootAction(String action) {
        for (String valid : VALID_ACTIONS) {
            if (valid.equals(action))
                return true;
        }
        return false;
    }
}