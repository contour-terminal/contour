// SPDX-License-Identifier: Apache-2.0
import QtMultimedia
import QtQuick

/// Encapsulates the multimedia components for playing the terminal bell sound.
/// Loaded lazily via a Loader to avoid the ~2s startup penalty from QtMultimedia
/// driver probing (FFmpeg/VDPAU/VA-API).
Item {
    /// The URL of the sound to play. Alias directly to the MediaPlayer's source
    /// to avoid intermediate URL resolution from a `property url`.
    property alias source: bellPlayer.source

    /// Plays the bell sound at the given volume.
    /// @param volume The playback volume (0.0 to 1.0).
    function play(volume) {
        if (bellPlayer.source == "")
            return;
        if (bellPlayer.playbackState === MediaPlayer.PlayingState)
            return;

        bellAudioOutput.volume = volume;
        bellPlayer.play();
    }

    AudioOutput {
        id: bellAudioOutput
    }

    MediaPlayer {
        id: bellPlayer
        audioOutput: bellAudioOutput
    }
}
