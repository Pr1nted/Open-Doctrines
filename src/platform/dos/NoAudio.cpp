// raylib's audio API, as nothing: the DOS build has no audio device.
//
// raylib plays sound through miniaudio, and miniaudio's device layer needs
// threads -- which DJGPP does not have (its libstdc++ is built without
// gthreads, and there is no pthread). So the DOS build compiles raylib without
// its audio module (SUPPORT_MODULE_RAUDIO OFF) and links these instead, the
// same no-ops the dedicated server links (src/server/ServerRaylibStubs.cpp,
// where they were taken from). IsAudioDeviceReady() is false, which is the
// state src/Audio.cpp already handles for a machine whose sound card is gone.
//
// A Sound Blaster mixer would replace this file, not the game's audio code.

#include "raylib.h"

void AttachAudioMixedProcessor(AudioCallback processor) { }
void AttachAudioStreamProcessor(AudioStream stream, AudioCallback processor) { }
void CloseAudioDevice(void) { }
void DetachAudioMixedProcessor(AudioCallback processor) { }
void DetachAudioStreamProcessor(AudioStream stream, AudioCallback processor) { }
bool ExportWave(Wave wave, const char *fileName) { return false; }
bool ExportWaveAsCode(Wave wave, const char *fileName) { return false; }
float GetMasterVolume(void) { return 0; }
float GetMusicTimeLength(Music music) { return 0; }
float GetMusicTimePlayed(Music music) { return 0; }
void InitAudioDevice(void) { }
bool IsAudioDeviceReady(void) { return false; }
bool IsAudioStreamPlaying(AudioStream stream) { return false; }
bool IsAudioStreamProcessed(AudioStream stream) { return false; }
bool IsAudioStreamValid(AudioStream stream) { return false; }
bool IsMusicStreamPlaying(Music music) { return false; }
bool IsMusicValid(Music music) { return false; }
bool IsSoundPlaying(Sound sound) { return false; }
bool IsSoundValid(Sound sound) { return false; }
bool IsWaveValid(Wave wave) { return false; }
AudioStream LoadAudioStream(unsigned int sampleRate, unsigned int sampleSize, unsigned int channels) { return AudioStream{}; }
Music LoadMusicStream(const char *fileName) { return Music{}; }
Music LoadMusicStreamFromMemory(const char *fileType, const unsigned char *data, int dataSize) { return Music{}; }
Sound LoadSound(const char *fileName) { return Sound{}; }
Sound LoadSoundAlias(Sound source) { return Sound{}; }
Sound LoadSoundFromWave(Wave wave) { return Sound{}; }
Wave LoadWave(const char *fileName) { return Wave{}; }
Wave LoadWaveFromMemory(const char *fileType, const unsigned char *fileData, int dataSize) { return Wave{}; }
float * LoadWaveSamples(Wave wave) { return nullptr; }
void PauseAudioStream(AudioStream stream) { }
void PauseMusicStream(Music music) { }
void PauseSound(Sound sound) { }
void PlayAudioStream(AudioStream stream) { }
void PlayMusicStream(Music music) { }
void PlaySound(Sound sound) { }
void ResumeAudioStream(AudioStream stream) { }
void ResumeMusicStream(Music music) { }
void ResumeSound(Sound sound) { }
void SeekMusicStream(Music music, float position) { }
void SetAudioStreamBufferSizeDefault(int size) { }
void SetAudioStreamCallback(AudioStream stream, AudioCallback callback) { }
void SetAudioStreamPan(AudioStream stream, float pan) { }
void SetAudioStreamPitch(AudioStream stream, float pitch) { }
void SetAudioStreamVolume(AudioStream stream, float volume) { }
void SetMasterVolume(float volume) { }
void SetMusicPan(Music music, float pan) { }
void SetMusicPitch(Music music, float pitch) { }
void SetMusicVolume(Music music, float volume) { }
void SetSoundPan(Sound sound, float pan) { }
void SetSoundPitch(Sound sound, float pitch) { }
void SetSoundVolume(Sound sound, float volume) { }
void StopAudioStream(AudioStream stream) { }
void StopMusicStream(Music music) { }
void StopSound(Sound sound) { }
void UnloadAudioStream(AudioStream stream) { }
void UnloadMusicStream(Music music) { }
void UnloadSound(Sound sound) { }
void UnloadSoundAlias(Sound alias) { }
void UnloadWave(Wave wave) { }
void UnloadWaveSamples(float *samples) { }
void UpdateAudioStream(AudioStream stream, const void *data, int frameCount) { }
void UpdateMusicStream(Music music) { }
void UpdateSound(Sound sound, const void *data, int frameCount) { }
Wave WaveCopy(Wave wave) { return Wave{}; }
void WaveCrop(Wave *wave, int initFrame, int finalFrame) { }
void WaveFormat(Wave *wave, int sampleRate, int sampleSize, int channels) { }
