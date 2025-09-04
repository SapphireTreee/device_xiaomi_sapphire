/*
 * Copyright (C) 2023 The PixelOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package org.pixelexperience.xiaomi.voipfix;

import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.media.AudioManager;
import android.media.AudioSystem;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;
import android.os.UserHandle;
import android.telephony.PhoneStateListener;
import android.telephony.TelephonyManager;
import android.util.Log;
import android.view.KeyEvent;
import androidx.annotation.NonNull; // Corrected import for NonNull

/**
 * VoIPFixService - automatically triggers volume adjustments during VoIP calls
 * to resolve muted audio issues on Xiaomi SM8350 devices
 */
public class VoIPFixService extends Service implements SensorEventListener {

    private static final String TAG = "XiaomiVoIPFix";
    private static final boolean DEBUG = true;

    private static final int SENSOR_SENSITIVITY = 4;

    private SensorManager mSensorManager;
    private AudioManager mAudioManager;
    private TelephonyManager mTelephonyManager;

    private int originalVolume;
    private boolean mIsFixApplied = false;
    private boolean mPendingSpeakerFix = false;

    private Handler mHandler;

    private final BroadcastReceiver mReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();
            if (action == null) return;

            switch (action) {
                case Intent.ACTION_SCREEN_ON:
                    if (DEBUG) log("Screen turned on");
                    // Restore to speakerphone when screen is on to avoid audio routing issues
                    // This is a common bug on some ROMs, so we'll force it here
                    if (mIsFixApplied) {
                        setSpeakerphone(true);
                    }
                    break;
                case Intent.ACTION_SCREEN_OFF:
                    if (DEBUG) log("Screen turned off");
                    if (mIsFixApplied) {
                        setSpeakerphone(false);
                    }
                    break;
                case "org.pixelexperience.xiaomi.voipfix.ACTION_APPLY_FIX":
                    // This action is for a specific scenario where we need to apply the fix
                    // after a headset is plugged in or out, but we don't have direct access
                    // to those events, so we'll rely on the audio change broadcast
                    applyVolumeFix();
                    break;
            }
        }
    };

    private void applyVolumeFix() {
        int currentVolume = mAudioManager.getStreamVolume(AudioManager.STREAM_VOICE_CALL);
        int maxVolume = mAudioManager.getStreamMaxVolume(AudioManager.STREAM_VOICE_CALL);
        log("Current voice call volume: " + currentVolume + " out of " + maxVolume);

        if (currentVolume > maxVolume / 2) {
            log("Current volume is above half, decreasing then restoring");
            mAudioManager.setStreamVolume(
                    AudioManager.STREAM_VOICE_CALL,
                    maxVolume / 2,
                    0);
            
            // Wait a moment before restoring
            mHandler.postDelayed(() -> {
                mAudioManager.setStreamVolume(
                        AudioManager.STREAM_VOICE_CALL,
                        originalVolume,
                        0);
                mIsFixApplied = true;
                mPendingSpeakerFix = false;
                log("Volume fix applied and restored to: " + originalVolume);
            }, 300);
        } else {
            // We're at or below half volume, so increase then decrease
            log("Current volume: " + currentVolume + ", increasing then restoring");
            mAudioManager.adjustStreamVolume(
                    AudioManager.STREAM_VOICE_CALL,
                    AudioManager.ADJUST_RAISE,
                    0);
            
            // Wait a moment before restoring
            mHandler.postDelayed(() -> {
                mAudioManager.setStreamVolume(
                        AudioManager.STREAM_VOICE_CALL,
                        originalVolume,
                        0);
                mIsFixApplied = true;
                mPendingSpeakerFix = false;
                log("Volume fix applied and restored to: " + originalVolume);
            }, 300);
        }
    }

    private final TelephonyManager.TelephonyCallback callStateCallback = new TelephonyManager.TelephonyCallback() {
        @Override
        public void onCallStateChanged(int state) {
            log("onCallStateChanged: " + state);
            if (state == TelephonyManager.CALL_STATE_IDLE) {
                log("Call ended, restoring audio state");
                restoreAudioState();
            }
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();

        log("VoIPFixService created");
        mAudioManager = getSystemService(AudioManager.class);
        mTelephonyManager = getSystemService(TelephonyManager.class);
        mSensorManager = getSystemService(SensorManager.class);
        mHandler = new Handler(Looper.getMainLooper());

        IntentFilter filter = new IntentFilter();
        filter.addAction(Intent.ACTION_SCREEN_ON);
        filter.addAction(Intent.ACTION_SCREEN_OFF);
        registerReceiver(mReceiver, filter);
        
        // This is where we will listen for telephony events to detect VoIP calls
        mTelephonyManager.registerTelephonyCallback(getMainLooper(), callStateCallback);
        
        // Register proximity sensor listener to handle automatic speakerphone
        Sensor proximitySensor = mSensorManager.getDefaultSensor(Sensor.TYPE_PROXIMITY);
        if (proximitySensor != null) {
            mSensorManager.registerListener(this, proximitySensor, SensorManager.SENSOR_DELAY_NORMAL);
        }
    }

    private void restoreAudioState() {
        if (!mIsFixApplied) {
            return;
        }

        log("Restoring audio state");
        // Restore to original audio state if fix was applied
        mAudioManager.setStreamVolume(AudioManager.STREAM_VOICE_CALL, originalVolume, 0);
        mIsFixApplied = false;
        mPendingSpeakerFix = false;
    }

    private void setSpeakerphone(boolean enabled) {
        if (enabled) {
            mAudioManager.setMode(AudioManager.MODE_IN_CALL);
            mAudioManager.setSpeakerphoneOn(true);
            log("Speakerphone enabled");
        } else {
            mAudioManager.setSpeakerphoneOn(false);
            mAudioManager.setMode(AudioManager.MODE_IN_COMMUNICATION);
            log("Speakerphone disabled, using earpiece");
        }
    }

    @Override
    public void onSensorChanged(SensorEvent event) {
        if (event.sensor.getType() == Sensor.TYPE_PROXIMITY) {
            float distance = event.values[0];
            log("Proximity sensor changed: " + distance);
            if (distance < SENSOR_SENSITIVITY) {
                log("Proximity sensor triggered, switching to earpiece");
                setSpeakerphone(false);
            } else {
                log("Proximity sensor un-triggered, switching to speakerphone");
                setSpeakerphone(true);
            }
        }
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {
        // Not used, but required by SensorEventListener
    }

    @Override
    public void onDestroy() {
        // Unregister listeners
        mSensorManager.unregisterListener(this);
        mTelephonyManager.unregisterTelephonyCallback(callStateCallback);
        unregisterReceiver(mReceiver);
        super.onDestroy();
        log("VoIPFix Service destroyed");
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && intent.getAction() != null) {
            log("Received action: " + intent.getAction());
        }
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void log(String msg) {
        if (DEBUG) {
            Log.d(TAG, msg);
        }
    }
}
