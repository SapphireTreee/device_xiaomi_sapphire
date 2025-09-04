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

/**
 * VoIPFixService - automatically triggers volume adjustments during VoIP calls
 * to resolve muted audio issues on Xiaomi SM8350 devices
 */
public class VoIPFixService extends Service implements SensorEventListener {
    private static final String TAG = "XiaomiVoIPFix";
    private static final boolean DEBUG = true;

    private final Handler mHandler = new Handler(Looper.getMainLooper());

    private AudioManager mAudioManager;
    private SensorManager mSensorManager;
    private Sensor mProximitySensor;
    private TelephonyManager mTelephonyManager;
    private PowerReceiver mReceiver;

    private boolean mIsFixApplied = false;
    private boolean mPendingSpeakerFix = false;
    private int originalVolume;

    // Use PhoneStateListener for compatibility with older Android versions
    private final PhoneStateListener callStateListener = new PhoneStateListener() {
        @Override
        public void onCallStateChanged(int state, String phoneNumber) {
            log("PhoneStateListener: onCallStateChanged: state=" + state + ", number=" + phoneNumber);
            switch (state) {
                case TelephonyManager.CALL_STATE_RINGING:
                case TelephonyManager.CALL_STATE_OFFHOOK:
                    // Stop the fix when a standard call is active
                    if (mIsFixApplied) {
                        log("Standard call detected, restoring volume and disabling fix");
                        restoreVolumeFix();
                    }
                    break;
                case TelephonyManager.CALL_STATE_IDLE:
                    // No need to do anything, the fix is already disabled by this point
                    break;
            }
        }
    };
    

    @Override
    public void onCreate() {
        super.onCreate();
        log("VoIPFix Service created");

        mAudioManager = getSystemService(AudioManager.class);
        mSensorManager = getSystemService(SensorManager.class);
        mTelephonyManager = getSystemService(TelephonyManager.class);
        mProximitySensor = mSensorManager.getDefaultSensor(Sensor.TYPE_PROXIMITY);

        // Register the proximity sensor listener
        if (mProximitySensor != null) {
            mSensorManager.registerListener(this, mProximitySensor, SensorManager.SENSOR_DELAY_NORMAL);
        } else {
            log("Proximity sensor not found on this device");
        }

        // Register the call state listener
        mTelephonyManager.listen(callStateListener, PhoneStateListener.LISTEN_CALL_STATE);

        // Register a BroadcastReceiver for screen on/off events
        mReceiver = new PowerReceiver();
        IntentFilter filter = new IntentFilter();
        filter.addAction(Intent.ACTION_SCREEN_ON);
        filter.addAction(Intent.ACTION_SCREEN_OFF);
        registerReceiver(mReceiver, filter);
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

    @Override
    public void onDestroy() {
        super.onDestroy();
        log("VoIPFix Service destroyed");
        // Unregister listeners and receiver
        mSensorManager.unregisterListener(this);
        mTelephonyManager.listen(callStateListener, PhoneStateListener.LISTEN_NONE);
        unregisterReceiver(mReceiver);
    }
    
    @Override
    public void onSensorChanged(SensorEvent event) {
        if (event.sensor.getType() == Sensor.TYPE_PROXIMITY) {
            float distance = event.values[0];
            log("Proximity sensor event: distance=" + distance);

            // Trigger the volume fix when the proximity sensor detects an object nearby (e.g., face to ear)
            // and an audio stream is active.
            if (distance < mProximitySensor.getMaximumRange() && AudioSystem.is ;VoIPStreamActive()) {
                log("Proximity sensor triggered and VoIP stream is active. Applying volume fix.");
                applyVolumeFix();
            }
        }
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {
        // Not used, but required for SensorEventListener interface
    }

    /**
     * Applies the volume fix by slightly adjusting the volume up and down.
     * This is a workaround to "wake up" the audio driver.
     */
    private void applyVolumeFix() {
        if (mIsFixApplied || mPendingSpeakerFix) {
            log("Fix already applied or pending, skipping.");
            return;
        }

        originalVolume = mAudioManager.getStreamVolume(AudioManager.STREAM_VOICE_CALL);
        log("Original volume before fix: " + originalVolume);
        
        // This is the main workaround: adjust volume up and down
        mAudioManager.adjustStreamVolume(
                AudioManager.STREAM_VOICE_CALL,
                AudioManager.ADJUST_RAISE,
                0);

        mPendingSpeakerFix = true;
        
        // Post a delayed task to restore the original volume
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

    /**
     * Restores the volume to the original value after the fix has been applied.
     */
    private void restoreVolumeFix() {
        if (!mIsFixApplied) {
            log("Fix not applied, skipping restore.");
            return;
        }

        mAudioManager.setStreamVolume(
                AudioManager.STREAM_VOICE_CALL,
                originalVolume,
                0);
        mIsFixApplied = false;
        log("Volume restored to original value: " + originalVolume);
    }

    private void log(String msg) {
        if (DEBUG) {
            Log.d(TAG, msg);
        }
    }

    private class PowerReceiver extends BroadcastReceiver {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (intent.getAction().equals(Intent.ACTION_SCREEN_OFF)) {
                log("Screen is off, applying volume fix if needed.");
                // This is an additional check for cases where the screen turns off during a VoIP call
                if (AudioSystem.isVoIPStreamActive()) {
                    applyVolumeFix();
                }
            }
        }
    }
}
