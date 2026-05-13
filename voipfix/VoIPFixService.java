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

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.media.AudioManager;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.telephony.TelephonyManager;
import android.util.Log;

import java.util.concurrent.atomic.AtomicBoolean;

/**
 * VoIPFixService - automatically triggers volume adjustments during VoIP calls
 * to resolve muted audio issues on Xiaomi SM6225 devices
 */
public class VoIPFixService extends Service {
    private static final String TAG = "XiaomiVoIPFix";
    private static final boolean DEBUG = false;

    private static final int NOTIFICATION_ID = 1;
    private static final String CHANNEL_ID = "voipfix_service";
    private static final int MAX_FIX_ATTEMPTS = 3;

    private AudioManager mAudioManager;
    private Handler mHandler;
    
    private boolean mVoIPCallActive = false;
    private boolean mSpeakerActive = false;
    private boolean mIsFixApplied = false;
    private boolean mPendingSpeakerFix = false;
    private long mLastSpeakerChange = 0;
    private int mFixAttemptCount = 0;

    private final AtomicBoolean mFixInProgress = new AtomicBoolean(false);

    private final BroadcastReceiver mReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();
            
            if (AudioManager.STREAM_DEVICES_CHANGED_ACTION.equals(action)) {
                int streamType = intent.getIntExtra(AudioManager.EXTRA_VOLUME_STREAM_TYPE, -1);
                
                if (mVoIPCallActive) {
                    boolean currentSpeakerState = mAudioManager.isSpeakerphoneOn();
                    
                    if (mSpeakerActive != currentSpeakerState) {
                        mSpeakerActive = currentSpeakerState;
                        log("Speaker mode changed to: " + mSpeakerActive);
                        
                        mPendingSpeakerFix = true;
                        mLastSpeakerChange = System.currentTimeMillis();
                        mIsFixApplied = false;
                        mFixAttemptCount = 0;
                        
                        scheduleMultipleFixes();
                    }
                }
            } else if (TelephonyManager.ACTION_PHONE_STATE_CHANGED.equals(action)) {
                String state = intent.getStringExtra(TelephonyManager.EXTRA_STATE);
                if (TelephonyManager.EXTRA_STATE_OFFHOOK.equals(state)) {
                    mVoIPCallActive = true;
                    mSpeakerActive = mAudioManager.isSpeakerphoneOn();
                    log("Call is active, monitoring for VoIP streams");
                    mHandler.postDelayed(() -> applyVolumeButtonFix(), 1000);
                } else if (TelephonyManager.EXTRA_STATE_IDLE.equals(state)) {
                    resetState();
                    log("Call ended, resetting VoIP fix state");
                }
            }
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        log("VoIPFix Service starting");

        startForeground(NOTIFICATION_ID, buildNotification());
        
        mAudioManager = (AudioManager) getSystemService(Context.AUDIO_SERVICE);
        mHandler = new Handler(Looper.getMainLooper());
        
        IntentFilter filter = new IntentFilter();
        filter.addAction(TelephonyManager.ACTION_PHONE_STATE_CHANGED);
        filter.addAction(AudioManager.STREAM_DEVICES_CHANGED_ACTION);
        filter.addAction(AudioManager.ACTION_AUDIO_BECOMING_NOISY);
        filter.addAction(AudioManager.STREAM_MUTE_CHANGED_ACTION);
        registerReceiver(mReceiver, filter);
        
        mHandler.postDelayed(new Runnable() {
            @Override
            public void run() {
                checkVoIPAndSpeakerState();
                mHandler.postDelayed(this, 500);
            }
        }, 500);
    }
    
    private Notification buildNotification() {
        NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        
        NotificationChannel channel = new NotificationChannel(
                CHANNEL_ID,
                "VoIP Audio Fix",
                NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("Monitoring VoIP audio routing");
        nm.createNotificationChannel(channel);
        
        return new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle("Xiaomi VoIP Fix")
                .setContentText("Monitoring call audio state")
                .setSmallIcon(android.R.drawable.ic_btn_speak_now)
                .setOngoing(true)
                .build();
    }
    
    private void checkVoIPAndSpeakerState() {
        int mode = mAudioManager.getMode();
        if (mode == AudioManager.MODE_IN_COMMUNICATION) {
            if (!mVoIPCallActive) {
                log("VoIP activity detected via audio mode");
                mVoIPCallActive = true;
                mSpeakerActive = mAudioManager.isSpeakerphoneOn();
                mHandler.postDelayed(() -> applyVolumeButtonFix(), 1000);
            } else {
                boolean currentSpeakerState = mAudioManager.isSpeakerphoneOn();
                if (mSpeakerActive != currentSpeakerState) {
                    log("Speaker change detected in polling: " + currentSpeakerState);
                    mSpeakerActive = currentSpeakerState;
                    mIsFixApplied = false;
                    mPendingSpeakerFix = true;
                    mLastSpeakerChange = System.currentTimeMillis();
                    mFixAttemptCount = 0;
                    scheduleMultipleFixes();
                }
                
                if (mPendingSpeakerFix && 
                    System.currentTimeMillis() - mLastSpeakerChange > 300 &&
                    !mIsFixApplied) {
                    applyVolumeButtonFix();
                }
            }
        } else if (mVoIPCallActive && mode != AudioManager.MODE_IN_CALL) {
            resetState();
            log("VoIP activity ended, resetting fix state");
        }
    }
    
    private void resetState() {
        mVoIPCallActive = false;
        mIsFixApplied = false;
        mPendingSpeakerFix = false;
        mFixInProgress.set(false);
        mFixAttemptCount = 0;
    }
    
    private void scheduleMultipleFixes() {
        mHandler.postDelayed(() -> maybeApplyFix(), 300);
        mHandler.postDelayed(() -> maybeApplyFix(), 600);
        mHandler.postDelayed(() -> maybeApplyFix(), 1000);
    }
    
    private void maybeApplyFix() {
        if (mPendingSpeakerFix && !mIsFixApplied && mFixAttemptCount < MAX_FIX_ATTEMPTS) {
            applyVolumeButtonFix();
        }
    }
    
    private void applyVolumeButtonFix() {
        if (!mVoIPCallActive) return;
        
        if (!mFixInProgress.compareAndSet(false, true)) {
            log("Fix already in progress, skipping");
            return;
        }
        
        mFixAttemptCount++;
        log("Applying volume button fix (attempt " + mFixAttemptCount + ")");
        
        int currentVolume = mAudioManager.getStreamVolume(AudioManager.STREAM_VOICE_CALL);
        int maxVolume = mAudioManager.getStreamMaxVolume(AudioManager.STREAM_VOICE_CALL);
        final int originalVolume = currentVolume;
        
        if (currentVolume > maxVolume / 2) {
            log("Current volume: " + currentVolume + ", decreasing then restoring");
            mAudioManager.adjustStreamVolume(
                    AudioManager.STREAM_VOICE_CALL,
                    AudioManager.ADJUST_LOWER,
                    0);
            
            mHandler.postDelayed(() -> {
                mAudioManager.setStreamVolume(
                        AudioManager.STREAM_VOICE_CALL,
                        originalVolume,
                        0);
                mIsFixApplied = true;
                mPendingSpeakerFix = false;
                mFixInProgress.set(false);
                log("Volume fix applied and restored to: " + originalVolume);
            }, 300);
        } else {
            log("Current volume: " + currentVolume + ", increasing then restoring");
            mAudioManager.adjustStreamVolume(
                    AudioManager.STREAM_VOICE_CALL,
                    AudioManager.ADJUST_RAISE,
                    0);
            
            mHandler.postDelayed(() -> {
                mAudioManager.setStreamVolume(
                        AudioManager.STREAM_VOICE_CALL,
                        originalVolume,
                        0);
                mIsFixApplied = true;
                mPendingSpeakerFix = false;
                mFixInProgress.set(false);
                log("Volume fix applied and restored to: " + originalVolume);
            }, 300);
        }
    }

    @Override
    public void onDestroy() {
        unregisterReceiver(mReceiver);
        super.onDestroy();
        log("VoIPFix Service destroyed");
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        startForeground(NOTIFICATION_ID, buildNotification());
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
