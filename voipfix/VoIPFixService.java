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
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.telephony.TelephonyCallback;
import android.telephony.TelephonyManager;
import android.util.Log;

import androidx.annotation.NonNull;

/**
 * VoIPFixService - automatically triggers volume adjustments and handles proximity sensor
 * to resolve audio and display issues during VoIP calls on certain devices.
 */
public class VoIPFixService extends Service {

    private static final String TAG = "VoIPFixService";

    // Managers and Handlers
    private TelephonyManager telephonyManager;
    private AudioManager audioManager;
    private SensorManager sensorManager;
    private PowerManager powerManager;
    private Handler handler;

    // Listeners and Receivers
    private CallStateCallback callStateCallback;
    private AudioDeviceReceiver audioDeviceReceiver;
    private SpeakerphoneReceiver speakerphoneReceiver;
    private ProximitySensorListener proximitySensorListener;

    // Wake lock to keep the screen off when near the ear
    private PowerManager.WakeLock proximityWakeLock;

    @Override
    public void onCreate() {
        super.onCreate();
        Log.d(TAG, "Service onCreate");

        // Initialize managers
        telephonyManager = (TelephonyManager) getSystemService(Context.TELEPHONY_SERVICE);
        audioManager = (AudioManager) getSystemService(Context.AUDIO_SERVICE);
        sensorManager = (SensorManager) getSystemService(Context.SENSOR_SERVICE);
        powerManager = (PowerManager) getSystemService(Context.POWER_SERVICE);
        handler = new Handler(Looper.getMainLooper());

        // Get the proximity sensor
        Sensor proximitySensor = sensorManager.getDefaultSensor(Sensor.TYPE_PROXIMITY);

        // Initialize and register callbacks/receivers
        callStateCallback = new CallStateCallback();
        audioDeviceReceiver = new AudioDeviceReceiver();
        speakerphoneReceiver = new SpeakerphoneReceiver();
        proximitySensorListener = new ProximitySensorListener();

        if (telephonyManager != null) {
            telephonyManager.registerTelephonyCallback(handler.getMainLooper(), callStateCallback);
        }

        // Register receiver for Bluetooth SCO audio state changes
        IntentFilter scoFilter = new IntentFilter(AudioManager.ACTION_SCO_AUDIO_STATE_UPDATED);
        registerReceiver(audioDeviceReceiver, scoFilter);

        // Register receiver for speakerphone state changes
        IntentFilter speakerphoneFilter = new IntentFilter(AudioManager.ACTION_SPEAKERPHONE_STATE_CHANGED);
        registerReceiver(speakerphoneReceiver, speakerphoneFilter);

        if (proximitySensor != null) {
            sensorManager.registerListener(proximitySensorListener, proximitySensor, SensorManager.SENSOR_DELAY_NORMAL);
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Log.d(TAG, "Service onStartCommand");
        return START_STICKY; // Service will be restarted if it is killed by the system
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        Log.d(TAG, "Service onDestroy");
        // Unregister listeners and release wake lock to prevent memory leaks and issues
        if (telephonyManager != null && callStateCallback != null) {
            telephonyManager.unregisterTelephonyCallback(callStateCallback);
        }
        if (audioDeviceReceiver != null) {
            unregisterReceiver(audioDeviceReceiver);
        }
        if (speakerphoneReceiver != null) {
            unregisterReceiver(speakerphoneReceiver);
        }
        if (proximitySensorListener != null) {
            sensorManager.unregisterListener(proximitySensorListener);
        }
        if (proximityWakeLock != null && proximityWakeLock.isHeld()) {
            proximityWakeLock.release();
        }
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    /**
     * Handles changes in the phone's call state using the modern TelephonyCallback API.
     */
    private class CallStateCallback extends TelephonyCallback implements TelephonyCallback.CallStateListener {
        @Override
        public void onCallStateChanged(int state) {
            Log.d(TAG, "onCallStateChanged: " + state);
            switch (state) {
                case TelephonyManager.CALL_STATE_OFFHOOK:
                    // A call is active, either incoming or outgoing.
                    Log.d(TAG, "Call is OFFHOOK. Setting audio mode to IN_COMMUNICATION.");
                    audioManager.setMode(AudioManager.MODE_IN_COMMUNICATION);
                    // Proximity sensor listener will be active now.
                    break;
                case TelephonyManager.CALL_STATE_IDLE:
                    // Call is ended.
                    Log.d(TAG, "Call is IDLE. Resetting audio mode.");
                    // Reset audio mode to normal after a short delay
                    handler.postDelayed(() -> {
                        if (telephonyManager.getCallState() == TelephonyManager.CALL_STATE_IDLE) {
                            audioManager.setMode(AudioManager.MODE_NORMAL);
                        }
                    }, 500); // 500ms delay to avoid conflicts
                    break;
            }
        }
    }

    /**
     * Handles changes in audio device state, specifically for Bluetooth SCO.
     */
    private class AudioDeviceReceiver extends BroadcastReceiver {
        @Override
        public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();
            if (AudioManager.ACTION_SCO_AUDIO_STATE_UPDATED.equals(action)) {
                int state = intent.getIntExtra(AudioManager.EXTRA_SCO_AUDIO_STATE, -1);
                Log.d(TAG, "SCO audio state updated: " + state);

                if (state == AudioManager.SCO_AUDIO_STATE_CONNECTED) {
                    // SCO (Bluetooth) audio is now connected.
                    handler.postDelayed(() -> {
                        int maxVolume = audioManager.getStreamMaxVolume(AudioManager.STREAM_VOICE_CALL);
                        Log.d(TAG, "SCO connected. Setting voice call volume to max: " + maxVolume);
                        audioManager.setStreamVolume(AudioManager.STREAM_VOICE_CALL, maxVolume, 0);
                    }, 200); // 200ms delay for stability
                }
            }
        }
    }

    /**
     * Handles speakerphone state changes to apply the volume fix.
     */
    private class SpeakerphoneReceiver extends BroadcastReceiver {
        @Override
        public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();
            if (AudioManager.ACTION_SPEAKERPHONE_STATE_CHANGED.equals(action)) {
                boolean isSpeakerphoneOn = audioManager.isSpeakerphoneOn();
                Log.d(TAG, "Speakerphone state changed: " + isSpeakerphoneOn);

                if (isSpeakerphoneOn) {
                    // Apply the volume fix when the speakerphone is turned on
                    handler.postDelayed(() -> {
                        int currentVolume = audioManager.getStreamVolume(AudioManager.STREAM_VOICE_CALL);
                        Log.d(TAG, "Applying volume fix: current volume is " + currentVolume);

                        // Temporarily decrease and then restore the volume
                        audioManager.adjustStreamVolume(AudioManager.STREAM_VOICE_CALL, AudioManager.ADJUST_LOWER, 0);
                        handler.postDelayed(() -> {
                            audioManager.setStreamVolume(AudioManager.STREAM_VOICE_CALL, currentVolume, 0);
                            Log.d(TAG, "Volume fix applied.");
                        }, 50); // Small delay to allow the adjustment to take effect
                    }, 200); // Delay to ensure the system has fully switched to speakerphone
                }
            }
        }
    }

    /**
     * Handles proximity sensor events to manage the screen's wake lock.
     */
    private class ProximitySensorListener implements SensorEventListener {
        @Override
        public void onSensorChanged(SensorEvent event) {
            // Only act if a call is active and the speakerphone is not on
            if (telephonyManager.getCallState() != TelephonyManager.CALL_STATE_OFFHOOK || audioManager.isSpeakerphoneOn()) {
                return;
            }

            if (event.sensor.getType() == Sensor.TYPE_PROXIMITY) {
                float distance = event.values[0];
                float maxDistance = event.sensor.getMaximumRange();

                if (distance < maxDistance) {
                    // Proximity sensor is close to the ear, acquire wake lock
                    if (proximityWakeLock == null) {
                        proximityWakeLock = powerManager.newWakeLock(
                                PowerManager.PROXIMITY_SCREEN_OFF_WAKE_LOCK,
                                TAG + ":proximity_lock");
                    }
                    if (!proximityWakeLock.isHeld()) {
                        proximityWakeLock.acquire(10 * 60 * 1000L /*10 minutes*/);
                        Log.d(TAG, "Proximity sensor near, acquiring wake lock.");
                    }
                } else {
                    // Proximity sensor is far, release wake lock
                    if (proximityWakeLock != null && proximityWakeLock.isHeld()) {
                        proximityWakeLock.release();
                        Log.d(TAG, "Proximity sensor far, releasing wake lock.");
                    }
                }
            }
        }

        @Override
        public void onAccuracyChanged(Sensor sensor, int accuracy) {
            // Not used
        }
    }
}