#include "PolyLab.h"

#include "../../Audio/Polyphonic/BasicPitchEvidence.h"
#include "../../Audio/Polyphonic/PolyEngine.h"
#include "../../Utils/Localization.h"
#include "../../Utils/PlatformPaths.h"
#include "../../Utils/UI/Theme.h"
#include "../Components/AppFont.h"
#include "../Components/DarkLookAndFeel.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include <vector>

namespace
{
using Channels = std::vector<std::vector<float>>;

constexpr int kKeysWidth = 56;
constexpr int kRulerHeight = 24;
constexpr int kMaxChannels = 2;
constexpr double kMaxSeconds = 10.0 * 60.0;
constexpr int kVariantGroup = 4021;

const juce::Colour kRowBlack(0xFF11141Bu);
const juce::Colour kEditInk(0xFF08262Au);

int pitchClass(int midi) { return ((midi % 12) + 12) % 12; }

bool isBlackKey(int midi)
{
    const int pc = pitchClass(midi);
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

float hzToMidi(float hz) { return 69.0f + 12.0f * std::log2(hz / 440.0f); }

juce::String noteName(float midi)
{
    static const char* const names[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    const int m = juce::jlimit(0, 127, juce::roundToInt(midi));
    return juce::String(names[pitchClass(m)]) + juce::String(m / 12 - 1);
}

juce::String formatTime(double seconds, int decimals = 1)
{
    const double scale = std::pow(10.0, decimals);
    seconds = std::round(std::max(0.0, seconds) * scale) / scale;   // so 59.97 s shows as 1:00.0, not 0:60.0
    const int minutes = static_cast<int>(std::floor(seconds / 60.0));
    const double rest = std::max(0.0, seconds - 60.0 * minutes);
    const int width = decimals > 0 ? 3 + decimals : 2;
    return juce::String(minutes) + ":" + juce::String(rest, decimals).paddedLeft('0', width);
}

juce::String formatStretch(float ratio)
{
    return juce::String(ratio, 2) + juce::String(juce::CharPointer_UTF8("\xc3\x97 ")) + TR("polylab.length");
}

juce::String formatShift(float semitones)
{
    const bool whole = std::abs(semitones - std::round(semitones)) < 0.005f;
    const auto value = whole ? juce::String(juce::roundToInt(semitones)) : juce::String(semitones, 2);
    return (semitones > 0.0f ? "+" : "") + value + " st";
}

double clampSafe(double v, double lo, double hi) { return lo > hi ? 0.5 * (lo + hi) : juce::jlimit(lo, hi, v); }

/** The Lab's copy of the audio: channels for rendering, their mix for analysis. */
struct SourceData
{
    std::shared_ptr<const Channels> channels;
    std::vector<float> mono;
    double sampleRate = 44100.0;
    juce::String name;

    int numSamples() const { return static_cast<int>(mono.size()); }
    double seconds() const { return sampleRate > 0.0 ? static_cast<double>(mono.size()) / sampleRate : 0.0; }
};

std::shared_ptr<const SourceData> makeSource(const PolyLabSource& in)
{
    auto src = std::make_shared<SourceData>();
    const int numChannels = std::min(kMaxChannels, in.audio.getNumChannels());
    const int n = in.audio.getNumSamples();
    auto channels = std::make_shared<Channels>(static_cast<size_t>(numChannels), std::vector<float>(static_cast<size_t>(n)));
    for (int c = 0; c < numChannels; ++c)
        std::copy(in.audio.getReadPointer(c), in.audio.getReadPointer(c) + n, (*channels)[static_cast<size_t>(c)].begin());
    src->mono.assign(static_cast<size_t>(n), 0.0f);
    for (const auto& ch : *channels)
        for (size_t i = 0; i < ch.size(); ++i)
            src->mono[i] += ch[i] / static_cast<float>(numChannels);
    src->channels = std::move(channels);
    src->sampleRate = in.sampleRate;
    src->name = in.name.isNotEmpty() ? in.name : juce::String("Untitled");
    return src;
}

/** Basic Pitch, loaded on first use. Only touched from the Lab's worker thread. */
struct EvidenceModel
{
    juce::File file;
    polyphonic::BasicPitchEvidence model;
    bool tried = false;
    bool loaded = false;

    bool ensureLoaded()
    {
        if (!tried)
        {
            tried = true;
            if (file.existsAsFile())
            {
#if JUCE_WINDOWS
                loaded = model.load(std::filesystem::path(file.getFullPathName().toWideCharPointer()));
#else
                loaded = model.load(std::filesystem::path(file.getFullPathName().toStdString()));
#endif
            }
        }
        return loaded;
    }
};

//==============================================================================
/**
 * A/B player on the app's audio device. Slots hold whole takes; switching slots
 * or replacing the one that is playing crossfades over 30 ms at the same position.
 * The audio thread only reads under a try-lock; buffers are released on the
 * message thread.
 */
class LabPlayer : public juce::AudioSource
{
public:
    enum Slot { original = 0, edited, solo, outgoing, numSlots };

    void prepareToPlay(int, double rate) override { deviceRate.store(rate > 0.0 ? rate : 44100.0); }
    void releaseResources() override {}

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        info.clearActiveBufferRegion();
        const juce::SpinLock::ScopedTryLockType sl(lock);
        if (!sl.isLocked())
            return;
        const double seekTo = pendingSeek.exchange(-1.0);
        if (seekTo >= 0.0)
            position = std::round(seekTo * sourceRate);   // whole samples: at equal rates playback is an exact copy
        if (crossfadePending.exchange(false))
        {
            previous = outgoing;
            fade = 0.0f;
        }
        int want = selected.load();
        if (slots[want] == nullptr)
            want = original;
        if (want != current)
        {
            previous = current;
            current = want;
            fade = 0.0f;
        }
        const Channels* now = slots[current].get();
        if (!playing.load() || now == nullptr || now->empty())
        {
            fade = 1.0f;   // a switch while stopped needs no crossfade
            renderAudition(info);
            return;
        }
        auditionOn = false;   // the take is playing: a note being held gives way
        const Channels* before = slots[previous].get();
        const double rate = deviceRate.load();
        const double step = sourceRate / rate;
        const float fadeStep = static_cast<float>(1.0 / (0.03 * rate));
        const int length = static_cast<int>((*now)[0].size());
        auto& out = *info.buffer;
        for (int i = 0; i < info.numSamples; ++i)
        {
            if (position >= static_cast<double>(length))
            {
                playing.store(false);
                position = 0.0;
                break;
            }
            const int i0 = static_cast<int>(position);
            const float frac = static_cast<float>(position - static_cast<double>(i0));
            for (int c = 0; c < out.getNumChannels(); ++c)
            {
                float v = read(now, c, i0, frac);
                if (fade < 1.0f)
                    v = fade * v + (1.0f - fade) * read(before, c, i0, frac);
                out.setSample(c, info.startSample + i, v);
            }
            if (fade < 1.0f)
                fade = std::min(1.0f, fade + fadeStep);
            position += step;
        }
        positionSeconds.store(position / sourceRate);
    }

    /**
     * One note heard on its own while it is held or dragged, over silence while the take is
     * stopped. `audio` starts at sample `start` of the take. While a note is sounding, a new
     * rendering of it takes over at the same point with a 10 ms crossfade, so a note dragged to
     * another pitch keeps sounding at the new one; otherwise it plays from `fromSample`.
     */
    void audition(std::shared_ptr<const Channels> audio, int start, int fromSample)
    {
        std::shared_ptr<const Channels> released[2];
        {
            const juce::SpinLock::ScopedLockType sl(lock);
            released[0] = std::move(auditionPrevious);
            if (auditionOn && !auditionStopping && auditionAudio != nullptr)
            {
                auditionPrevious = std::move(auditionAudio);
                auditionPreviousPos = auditionPos;
                auditionPos += static_cast<double>(auditionStart - start);
                auditionCross = 0.0f;
            }
            else
            {
                released[1] = std::move(auditionAudio);
                auditionPos = static_cast<double>(fromSample - start);
                auditionGain = 0.0f;
                auditionCross = 1.0f;
            }
            auditionAudio = std::move(audio);
            auditionStart = start;
            auditionOn = auditionAudio != nullptr && !auditionAudio->empty();
            auditionStopping = false;
        }
    }

    /** Fades the held note out (10 ms). */
    void stopAudition()
    {
        const juce::SpinLock::ScopedLockType sl(lock);
        auditionStopping = true;
    }

    void setSource(std::shared_ptr<const Channels> audio, double rate)
    {
        std::shared_ptr<const Channels> released[numSlots];
        std::shared_ptr<const Channels> releasedAudition[2];
        const double seconds = audio != nullptr && !audio->empty() && rate > 0.0
                                   ? static_cast<double>((*audio)[0].size()) / rate : 0.0;
        {
            const juce::SpinLock::ScopedLockType sl(lock);
            playing.store(false);
            auditionOn = false;
            releasedAudition[0] = std::move(auditionAudio);
            releasedAudition[1] = std::move(auditionPrevious);
            for (int s = 0; s < numSlots; ++s)
                released[s] = std::move(slots[s]);
            slots[original] = std::move(audio);
            sourceRate = rate > 0.0 ? rate : 44100.0;
            position = 0.0;
            current = previous = original;
            fade = 1.0f;
            crossfadePending.store(false);
            pendingSeek.store(-1.0);
        }
        positionSeconds.store(0.0);
        lengthSeconds.store(seconds);
    }

    void setSlot(int slot, std::shared_ptr<const Channels> audio)
    {
        std::shared_ptr<const Channels> released;
        {
            const juce::SpinLock::ScopedLockType sl(lock);
            if (slot == current && slots[slot] != nullptr && audio != nullptr)
            {
                released = std::move(slots[outgoing]);
                slots[outgoing] = std::move(slots[slot]);
                crossfadePending.store(true);
            }
            else
            {
                released = std::move(slots[slot]);
            }
            slots[slot] = std::move(audio);
        }
    }

    std::shared_ptr<const Channels> getSlot(int slot) const
    {
        const juce::SpinLock::ScopedLockType sl(lock);
        return slots[slot];
    }

    void select(int slot) { selected.store(slot); }
    int getSelected() const { return selected.load(); }
    void play() { playing.store(lengthSeconds.load() > 0.0); }
    void pause() { playing.store(false); }
    bool isPlaying() const { return playing.load(); }
    void seek(double seconds)
    {
        seconds = juce::jlimit(0.0, lengthSeconds.load(), seconds);
        pendingSeek.store(seconds);
        positionSeconds.store(seconds);
    }
    double getPosition() const { return positionSeconds.load(); }
    double getLength() const { return lengthSeconds.load(); }

private:
    /** The held note, written over the cleared buffer; called on the audio thread under the lock. */
    void renderAudition(const juce::AudioSourceChannelInfo& info)
    {
        const Channels* now = auditionAudio.get();
        if (!auditionOn || now == nullptr || now->empty())
            return;
        const Channels* before = auditionPrevious.get();
        const double rate = deviceRate.load();
        const double step = sourceRate / rate;
        const float ramp = static_cast<float>(1.0 / (0.01 * rate));
        const double length = static_cast<double>((*now)[0].size());
        auto& out = *info.buffer;
        for (int i = 0; i < info.numSamples; ++i)
        {
            if (auditionPos >= length || (auditionStopping && auditionGain <= 0.0f))
            {
                auditionOn = false;
                break;
            }
            const double p = std::max(0.0, auditionPos), q = std::max(0.0, auditionPreviousPos);
            const int i0 = static_cast<int>(p), j0 = static_cast<int>(q);
            const float frac = static_cast<float>(p - i0), prevFrac = static_cast<float>(q - j0);
            for (int c = 0; c < out.getNumChannels(); ++c)
            {
                float v = auditionPos < 0.0 ? 0.0f : read(now, c, i0, frac);
                if (auditionCross < 1.0f)
                    v = auditionCross * v + (1.0f - auditionCross) * read(before, c, j0, prevFrac);
                out.setSample(c, info.startSample + i, v * auditionGain);
            }
            auditionPos += step;
            auditionPreviousPos += step;
            auditionCross = std::min(1.0f, auditionCross + ramp);
            auditionGain = auditionStopping ? std::max(0.0f, auditionGain - ramp) : std::min(1.0f, auditionGain + ramp);
        }
    }

    /** Catmull-Rom interpolation; exact at whole-sample positions. */
    static float read(const Channels* audio, int channel, int i, float t)
    {
        if (audio == nullptr || audio->empty())
            return 0.0f;
        const auto& v = (*audio)[static_cast<size_t>(std::min(channel, static_cast<int>(audio->size()) - 1))];
        const int n = static_cast<int>(v.size());
        if (i < 0 || i >= n)
            return 0.0f;
        const auto at = [&](int k) { return v[static_cast<size_t>(juce::jlimit(0, n - 1, k))]; };
        const float y0 = at(i - 1), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
        if (t == 0.0f)
            return y1;
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + y1;
    }

    std::shared_ptr<const Channels> slots[numSlots];
    mutable juce::SpinLock lock;
    double sourceRate = 44100.0;
    double position = 0.0;
    int current = original;
    int previous = original;
    float fade = 1.0f;
    std::atomic<double> deviceRate { 44100.0 };
    std::atomic<double> pendingSeek { -1.0 };
    std::atomic<double> positionSeconds { 0.0 };
    std::atomic<double> lengthSeconds { 0.0 };
    std::atomic<int> selected { original };
    std::atomic<bool> playing { false };
    std::atomic<bool> crossfadePending { false };

    // the held note (under `lock`)
    std::shared_ptr<const Channels> auditionAudio, auditionPrevious;
    int auditionStart = 0;
    double auditionPos = 0.0, auditionPreviousPos = 0.0;
    float auditionGain = 0.0f, auditionCross = 1.0f;
    bool auditionOn = false, auditionStopping = false;
};

//==============================================================================
/** Speaker toggle: hear a note on its own while you hold or drag it. */
class AuditionButton : public juce::Button
{
public:
    AuditionButton() : juce::Button("Hear notes")
    {
        setClickingTogglesState(true);
        setWantsKeyboardFocus(false);
    }

    void paintButton(juce::Graphics& g, bool over, bool down) override
    {
        const auto b = getLocalBounds().toFloat().reduced(1.0f);
        const bool on = getToggleState();
        auto fill = on ? APP_COLOR_PRIMARY : APP_COLOR_SURFACE_RAISED;
        if (over)
            fill = fill.brighter(0.12f);
        if (down)
            fill = fill.darker(0.2f);
        g.setColour(fill);
        g.fillRoundedRectangle(b, 5.0f);
        g.setColour(APP_COLOR_BORDER);
        g.drawRoundedRectangle(b, 5.0f, 1.0f);

        const auto c = b.getCentre();
        const float s = std::min(b.getWidth(), b.getHeight()) * 0.1f;
        const float x0 = c.x - 3.0f * s;
        juce::Path speaker;
        speaker.startNewSubPath(x0, c.y - 1.1f * s);
        speaker.lineTo(x0 + 1.4f * s, c.y - 1.1f * s);
        speaker.lineTo(x0 + 3.2f * s, c.y - 2.8f * s);
        speaker.lineTo(x0 + 3.2f * s, c.y + 2.8f * s);
        speaker.lineTo(x0 + 1.4f * s, c.y + 1.1f * s);
        speaker.lineTo(x0, c.y + 1.1f * s);
        speaker.closeSubPath();
        const auto ink = on ? APP_COLOR_TEXT_PRIMARY : APP_COLOR_TEXT_MUTED;
        g.setColour(ink);
        g.fillPath(speaker);
        const float x1 = x0 + 4.4f * s;
        if (on)
        {
            for (float r : { 1.6f * s, 3.0f * s })
            {
                juce::Path wave;
                wave.addCentredArc(x1 - 0.6f * s, c.y, r, r, 0.0f, juce::MathConstants<float>::pi * 0.22f,
                                   juce::MathConstants<float>::pi * 0.78f, true);
                g.strokePath(wave, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
        }
        else
        {
            g.drawLine(x1, c.y - 1.3f * s, x1 + 2.6f * s, c.y + 1.3f * s, 1.4f);
            g.drawLine(x1, c.y + 1.3f * s, x1 + 2.6f * s, c.y - 1.3f * s, 1.4f);
        }
    }
};

//==============================================================================
class PlayButton : public juce::Button
{
public:
    PlayButton() : juce::Button("Play") { setWantsKeyboardFocus(false); }

    void setPlaying(bool shouldShowPause)
    {
        if (shouldShowPause != playing)
        {
            playing = shouldShowPause;
            repaint();
        }
    }

    void paintButton(juce::Graphics& g, bool over, bool down) override
    {
        const auto b = getLocalBounds().toFloat();
        const float d = std::min(b.getWidth(), b.getHeight()) - 2.0f;
        const auto circle = b.withSizeKeepingCentre(d, d);
        auto colour = APP_COLOR_PRIMARY;
        if (over)
            colour = colour.brighter(0.15f);
        if (down)
            colour = colour.darker(0.2f);
        if (!isEnabled())
            colour = colour.withMultipliedAlpha(0.4f);
        g.setColour(colour);
        g.fillEllipse(circle);
        g.setColour(APP_COLOR_TEXT_PRIMARY.withMultipliedAlpha(isEnabled() ? 1.0f : 0.5f));
        const float s = d * 0.2f;
        const auto c = circle.getCentre();
        if (playing)
        {
            g.fillRoundedRectangle(c.x - s * 0.95f, c.y - s, s * 0.7f, s * 2.0f, 1.2f);
            g.fillRoundedRectangle(c.x + s * 0.25f, c.y - s, s * 0.7f, s * 2.0f, 1.2f);
        }
        else
        {
            juce::Path tri;
            tri.addTriangle(c.x - s * 0.7f, c.y - s * 1.1f, c.x - s * 0.7f, c.y + s * 1.1f, c.x + s * 1.15f, c.y);
            g.fillPath(tri);
        }
    }

private:
    bool playing = false;
};

//==============================================================================
/**
 * Piano roll of the polyphonic engine's notes. Each note is a blob along its own f0
 * track whose thickness follows its loudness; edits are drawn in the teal
 * accent with a dashed outline where the note was.
 */
class NoteCanvas : public juce::Component
{
public:
    std::function<void()> onEditsChanging;
    std::function<void()> onEditsChanged;
    std::function<void()> onSelectionChanged;
    std::function<void(double)> onSeek;
    std::function<void()> onTogglePlay;
    std::function<void()> onToggleCompare;
    std::function<void(int)> onNotePressed;   // a note (or its right end) was grabbed
    std::function<void()> onNoteReleased;

    explicit NoteCanvas(std::map<int, polyphonic::NoteEdit>& editsToUse) : edits(editsToUse)
    {
        setWantsKeyboardFocus(true);
        setOpaque(true);
    }

    void setAnalysis(std::shared_ptr<const polyphonic::Analysis> newAnalysis, double durationSeconds)
    {
        analysis = std::move(newAnalysis);
        duration = durationSeconds;
        selection.clear();
        hovered = -1;
        numActive = 0;
        envelopes.clear();
        medians.clear();
        if (analysis != nullptr)
        {
            for (const auto& n : analysis->notes)
                numActive += n.active ? 1 : 0;
            envelopes.resize(analysis->notes.size());
            medians.resize(analysis->notes.size());
            for (size_t i = 0; i < analysis->notes.size(); ++i)
            {
                envelopes[i] = noteEnvelope(i);
                medians[i] = analysis->notes[i].medianMidi();
            }
        }
        autoFit = true;
        zoomToFit();
        repaint();
    }

    void setDuration(double seconds)
    {
        duration = seconds;
        autoFit = true;
        zoomToFit();
    }

    void setMessage(const juce::String& text)
    {
        if (text != message)
        {
            message = text;
            repaint();
        }
    }

    void setShowCandidates(bool show)
    {
        showCandidates = show;
        repaint();
    }

    void setPlayhead(double seconds, bool playing)
    {
        if (std::abs(seconds - playhead) < 1e-4)
            return;
        const float oldX = timeToX(playhead);
        playhead = seconds;
        const float newX = timeToX(playhead);
        const auto area = rollArea();
        if (playing && drag == Drag::none && (newX > area.getRight() || newX < area.getX()))
        {
            viewStart = playhead - 0.1 * area.getWidth() / pxPerSecond;
            clampView();
            repaint();
            return;
        }
        repaint(juce::Rectangle<int>(static_cast<int>(oldX) - 7, 0, 14, getHeight()));
        repaint(juce::Rectangle<int>(static_cast<int>(newX) - 7, 0, 14, getHeight()));
    }

    const std::set<int>& getSelection() const { return selection; }
    bool isDraggingNotes() const { return drag == Drag::notes || drag == Drag::stretch; }

    void selectAll()
    {
        selection.clear();
        for (int i = 0; i < numActive; ++i)
            selection.insert(i);
        selectionChanged();
        repaint();
    }

    void zoomToFit()
    {
        const auto area = rollArea();
        if (area.getWidth() < 10.0f || area.getHeight() < 10.0f)
            return;
        viewStart = 0.0;
        pxPerSecond = area.getWidth() / ((duration > 0.0 ? std::max(0.5, duration) : 10.0) * 1.02);
        float lo = 48.0f, hi = 72.0f;
        if (analysis != nullptr && numActive > 0)
        {
            lo = 200.0f;
            hi = -200.0f;
            for (int i = 0; i < numActive; ++i)
                for (size_t k = 0; k < analysis->notes[static_cast<size_t>(i)].f0.size(); ++k)
                {
                    const float m = midiAt(i, static_cast<int>(k));
                    lo = std::min(lo, m);
                    hi = std::max(hi, m);
                }
            if (hi < lo)
                lo = 48.0f, hi = 72.0f;
        }
        lo -= 3.0f;
        hi += 3.0f;
        pxPerSemitone = juce::jlimit(minPxPerSemitone(), 22.0f, area.getHeight() / std::max(12.0f, hi - lo));
        topMidi = 0.5f * (lo + hi) + 0.5f * area.getHeight() / pxPerSemitone;
        clampView();
        repaint();
    }

    void resized() override
    {
        if (autoFit)
            zoomToFit();
        else
            clampView();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(APP_COLOR_SURFACE);
        const auto area = rollArea();
        const auto clip = g.getClipBounds().toFloat();
        {
            juce::Graphics::ScopedSaveState save(g);
            g.reduceClipRegion(area.toNearestInt());
            paintGrid(g, area);
            if (analysis != nullptr)
            {
                if (showCandidates)
                    for (int i = numActive; i < static_cast<int>(analysis->notes.size()); ++i)
                        paintCandidate(g, i, clip);
                for (int i = 0; i < numActive; ++i)
                    if (selection.count(i) == 0)
                        paintNote(g, i, clip);
                for (int i : selection)
                    paintNote(g, i, clip);
                paintHoverLabel(g, area);
            }
            if (drag == Drag::band)
            {
                g.setColour(APP_COLOR_SELECTION_HIGHLIGHT);
                g.fillRect(band);
                g.setColour(APP_COLOR_SELECTION_HIGHLIGHT_STRONG);
                g.drawRect(band, 1.0f);
            }
            const float px = timeToX(playhead);
            g.setColour(APP_COLOR_TEXT_PRIMARY.withAlpha(0.85f));
            g.fillRect(px - 0.5f, area.getY(), 1.0f, area.getHeight());
        }
        paintKeys(g);
        paintRuler(g);
        if (message.isNotEmpty())
        {
            g.setColour(APP_COLOR_TEXT_MUTED);
            g.setFont(AppFont::getFont(15.0f));
            g.drawFittedText(message, area.reduced(40.0f).toNearestInt(), juce::Justification::centred, 3);
        }
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        const bool inRoll = rollArea().contains(e.position);
        const int edge = inRoll ? edgeAt(e.position) : -1;
        const int h = edge >= 0 ? edge : (inRoll ? noteAt(e.position) : -1);
        if (h != hovered)
        {
            hovered = h;
            repaint();
        }
        setMouseCursor(edge >= 0 ? juce::MouseCursor::LeftRightResizeCursor
                                 : (h >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor));
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (hovered >= 0)
        {
            hovered = -1;
            repaint();
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        const auto p = e.position;
        pendingSingle = -1;
        if (p.y < static_cast<float>(kRulerHeight) && p.x >= static_cast<float>(kKeysWidth))
        {
            drag = Drag::seek;
            seekTo(p.x);
            return;
        }
        if (p.x < static_cast<float>(kKeysWidth) || analysis == nullptr)
            return;
        const bool additive = e.mods.isShiftDown() || e.mods.isCommandDown();
        if (const int edge = additive ? -1 : edgeAt(p); edge >= 0)
        {
            if (selection != std::set<int>{ edge })
            {
                selection = { edge };
                selectionChanged();
            }
            drag = Drag::stretch;
            stretchNote = edge;
            dragStart = p;
            dragMoved = false;
            snapshot.clear();
            snapshot[edge] = editOf(edge);
            if (onNotePressed)
                onNotePressed(edge);
            repaint();
            return;
        }
        const int hit = noteAt(p);
        if (hit >= 0)
        {
            if (additive)
            {
                if (selection.erase(hit) == 0)
                    selection.insert(hit);
                selectionChanged();
                if (selection.count(hit) == 0)
                {
                    drag = Drag::none;
                    repaint();
                    return;
                }
            }
            else if (selection.count(hit) == 0)
            {
                selection = { hit };
                selectionChanged();
            }
            else
            {
                pendingSingle = hit;   // a click without a drag selects just this note
            }
            drag = Drag::notes;
            dragStart = p;
            dragMoved = false;
            timeEngaged = false;
            snapshot.clear();
            for (int i : selection)
                snapshot[i] = editOf(i);
            if (onNotePressed)
                onNotePressed(hit);
        }
        else
        {
            if (!additive && !selection.empty())
            {
                selection.clear();
                selectionChanged();
            }
            drag = Drag::band;
            dragStart = p;
            band = {};
            bandBase = selection;
        }
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (drag == Drag::seek)
        {
            seekTo(e.position.x);
            return;
        }
        if (drag == Drag::stretch)
        {
            const auto& n = analysis->notes[static_cast<size_t>(stretchNote)];
            const auto base = snapshot[stretchNote];
            const auto map = polyphonic::stretchMap(*analysis, stretchNote, base);
            const double framesPerSecond = analysis->params.sampleRate / analysis->params.hop;
            const double span = n.endFrame - map.anchor;
            if (span < 2.0)
                return;
            const double endFrame = (xToTime(e.position.x) - base.timeOffsetSeconds) * framesPerSecond;
            const double lastFrame = (duration - 0.05 - base.timeOffsetSeconds) * framesPerSecond;  // stay inside the take
            double ratio = (endFrame - map.anchor) / span;
            ratio = std::min(ratio, (lastFrame - map.anchor) / span);
            ratio = std::round(juce::jlimit(0.25, 4.0, ratio) * 100.0) / 100.0;
            auto& ed = edits[stretchNote];
            if (static_cast<float>(ratio) != ed.stretch)
            {
                ed.stretch = static_cast<float>(ratio);
                dragMoved = true;
                if (onEditsChanging)
                    onEditsChanging();
                repaint();
            }
            return;
        }
        if (drag == Drag::notes)
        {
            const float dx = e.position.x - dragStart.x;
            const float dy = e.position.y - dragStart.y;
            float semis = -dy / pxPerSemitone;
            semis = e.mods.isAltDown() ? std::round(semis * 100.0f) / 100.0f : std::round(semis);
            if (!timeEngaged && std::abs(dx) > 8.0f)
                timeEngaged = true;   // small sideways wobble while dragging pitch is ignored
            const double dt = timeEngaged ? dx / pxPerSecond : 0.0;
            bool changed = false;
            for (const auto& [i, base] : snapshot)
            {
                auto& ed = edits[i];
                const float s = juce::jlimit(-36.0f, 36.0f, base.semitones + semis);
                const double o = clampOffset(i, base.timeOffsetSeconds + dt);
                changed = changed || s != ed.semitones || o != ed.timeOffsetSeconds;
                ed.semitones = s;
                ed.timeOffsetSeconds = o;
            }
            if (changed)
            {
                dragMoved = true;
                pendingSingle = -1;
                if (onEditsChanging)
                    onEditsChanging();
                repaint();
            }
            return;
        }
        if (drag == Drag::band)
        {
            band = juce::Rectangle<float>(dragStart, e.position);
            selection = bandBase;
            for (int i = 0; i < numActive; ++i)
                if (noteBounds(i).intersects(band))
                    selection.insert(i);
            repaint();
        }
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if ((drag == Drag::notes || drag == Drag::stretch) && onNoteReleased)
            onNoteReleased();
        if (drag == Drag::stretch)
        {
            if (dragMoved)
                commit();
        }
        else if (drag == Drag::notes)
        {
            if (dragMoved)
                commit();
            else if (pendingSingle >= 0)
            {
                selection = { pendingSingle };
                selectionChanged();
            }
        }
        else if (drag == Drag::band)
        {
            selectionChanged();
        }
        drag = Drag::none;
        band = {};
        repaint();
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        const int hit = rollArea().contains(e.position) ? noteAt(e.position) : -1;
        if (hit >= 0)
        {
            if (edits.erase(hit) > 0)
                commit();
        }
        else if (rollArea().contains(e.position))
        {
            autoFit = true;
            zoomToFit();
        }
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const auto area = rollArea();
        if (e.mods.isCommandDown() || e.mods.isCtrlDown())
        {
            const double anchor = xToTime(e.position.x);
            const float amount = std::abs(w.deltaY) >= std::abs(w.deltaX) ? w.deltaY : w.deltaX;
            pxPerSecond = juce::jlimit(2.0, 5000.0, pxPerSecond * std::exp(2.0 * amount));
            viewStart = anchor - (e.position.x - kKeysWidth) / pxPerSecond;
        }
        else if (e.mods.isAltDown())
        {
            const float anchor = yToMidi(e.position.y);
            pxPerSemitone = juce::jlimit(minPxPerSemitone(), 40.0f, pxPerSemitone * std::exp(2.0f * w.deltaY));
            topMidi = anchor + (e.position.y - kRulerHeight) / pxPerSemitone;
        }
        else
        {
            float dx = w.deltaX, dy = w.deltaY;
            if (e.mods.isShiftDown() && dx == 0.0f)
                std::swap(dx, dy);
            viewStart -= dx * 0.5 * area.getWidth() / pxPerSecond;
            topMidi += dy * 12.0f;
        }
        autoFit = false;
        clampView();
        repaint();
    }

    void mouseMagnify(const juce::MouseEvent& e, float scale) override
    {
        const double anchor = xToTime(e.position.x);
        pxPerSecond = juce::jlimit(2.0, 5000.0, pxPerSecond * scale);
        viewStart = anchor - (e.position.x - kKeysWidth) / pxPerSecond;
        autoFit = false;
        clampView();
        repaint();
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        const auto mods = key.getModifiers();
        const int code = key.getKeyCode();
        const auto letter = [code](char c) {
            return juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(code)) == static_cast<juce::juce_wchar>(c);
        };
        if (code == juce::KeyPress::spaceKey)
        {
            if (onTogglePlay)
                onTogglePlay();
            return true;
        }
        if (letter('a') && mods.isCommandDown())
        {
            selectAll();
            return true;
        }
        if (letter('a') && !mods.isAnyModifierKeyDown())
        {
            if (onToggleCompare)
                onToggleCompare();
            return true;
        }
        if (code == juce::KeyPress::escapeKey)
        {
            selection.clear();
            selectionChanged();
            repaint();
            return true;
        }
        if (selection.empty())
            return false;
        if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
        {
            float d = mods.isShiftDown() ? 12.0f : (mods.isAltDown() ? 0.1f : 1.0f);
            if (code == juce::KeyPress::downKey)
                d = -d;
            for (int i : selection)
            {
                auto& ed = edits[i];
                ed.semitones = juce::jlimit(-36.0f, 36.0f, ed.semitones + d);
            }
            commit();
            return true;
        }
        if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
        {
            double d = mods.isAltDown() ? 0.01 : 0.05;
            if (code == juce::KeyPress::leftKey)
                d = -d;
            for (int i : selection)
            {
                auto& ed = edits[i];
                ed.timeOffsetSeconds = clampOffset(i, ed.timeOffsetSeconds + d);
            }
            commit();
            return true;
        }
        if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
        {
            bool allMuted = true;
            for (int i : selection)
                allMuted = allMuted && editOf(i).mute;
            for (int i : selection)
                edits[i].mute = !allMuted;
            commit();
            return true;
        }
        if (letter('r') && !mods.isAnyModifierKeyDown())
        {
            for (int i : selection)
                edits.erase(i);
            commit();
            return true;
        }
        return false;
    }

private:
    enum class Drag { none, notes, band, seek, stretch };

    struct Shape
    {
        juce::Path body;
        juce::Path curve;
        juce::Rectangle<float> bounds;
    };

    juce::Rectangle<float> rollArea() const
    {
        return getLocalBounds().toFloat().withTrimmedLeft(static_cast<float>(kKeysWidth)).withTrimmedTop(static_cast<float>(kRulerHeight));
    }
    float timeToX(double t) const { return static_cast<float>(kKeysWidth + (t - viewStart) * pxPerSecond); }
    double xToTime(float x) const { return viewStart + (x - kKeysWidth) / pxPerSecond; }
    float midiToY(float m) const { return kRulerHeight + (topMidi - m) * pxPerSemitone; }
    float yToMidi(float y) const { return topMidi - (y - kRulerHeight) / pxPerSemitone; }
    double frameSeconds(int frame) const { return analysis->frameToSeconds(frame); }
    float noteThickness() const { return juce::jlimit(4.0f, 16.0f, pxPerSemitone * 0.8f); }
    float minPxPerSemitone() const { return std::max(3.0f, rollArea().getHeight() / 96.0f); }

    void clampView()
    {
        const auto area = rollArea();
        if (area.getWidth() <= 0.0f || area.getHeight() <= 0.0f)
            return;
        const double visibleSeconds = area.getWidth() / pxPerSecond;
        viewStart = clampSafe(viewStart, -0.25 * visibleSeconds, std::max(0.0, duration - 0.75 * visibleSeconds));
        pxPerSemitone = std::max(pxPerSemitone, minPxPerSemitone());
        const float visibleSemis = area.getHeight() / pxPerSemitone;
        topMidi = static_cast<float>(clampSafe(topMidi, 12.0 + visibleSemis, 120.0));
    }

    polyphonic::NoteEdit editOf(int i) const
    {
        const auto it = edits.find(i);
        return it != edits.end() ? it->second : polyphonic::NoteEdit{};
    }

    float midiAt(int i, int k) const
    {
        const auto& n = analysis->notes[static_cast<size_t>(i)];
        const float f = k >= 0 && k < static_cast<int>(n.f0.size()) ? n.f0[static_cast<size_t>(k)] : 0.0f;
        return f > 1.0f ? hzToMidi(f) : medians[static_cast<size_t>(i)];
    }

    std::vector<float> noteEnvelope(size_t i) const
    {
        const auto& n = analysis->notes[i];
        std::vector<float> e(static_cast<size_t>(std::max(1, n.numFrames())), 0.0f);
        if (i < analysis->masks.size())
            for (const auto& pm : analysis->masks[i].partials)
            {
                const int k = pm.frame - n.startFrame;
                if (k >= 0 && k < static_cast<int>(e.size()))
                    e[static_cast<size_t>(k)] += pm.amplitude * pm.amplitude;
            }
        float peak = 0.0f;
        for (auto& v : e)
        {
            v = std::sqrt(v);
            peak = std::max(peak, v);
        }
        if (peak <= 0.0f)
        {
            std::fill(e.begin(), e.end(), 1.0f);
            return e;
        }
        for (auto& v : e)
            v = std::pow(v / peak, 0.6f);
        return e;
    }

    /** Seconds at which note i's frame (a source frame, possibly fractional) plays under edit e. */
    double playedAt(int i, const polyphonic::NoteEdit& e, double frame) const
    {
        return analysis->frameToSeconds(polyphonic::stretchMap(*analysis, i, e).outputFrame(frame)) + e.timeOffsetSeconds;
    }

    /** The note whose right end (the stretch handle) is under p, or -1. */
    int edgeAt(juce::Point<float> p) const
    {
        if (analysis == nullptr)
            return -1;
        const auto touches = [&](int i) {
            const auto e = editOf(i);
            const auto& n = analysis->notes[static_cast<size_t>(i)];
            if (e.mute || n.f0.empty() || n.endFrame - polyphonic::stretchMap(*analysis, i, e).anchor < 2.0)
                return false;
            const float x = timeToX(playedAt(i, e, n.endFrame));
            const float y = midiToY(midiAt(i, static_cast<int>(n.f0.size()) - 1) + e.semitones);
            return std::abs(p.x - x) <= 5.0f && std::abs(p.y - y) <= std::max(6.0f, 0.5f * noteThickness() + 2.0f);
        };
        for (auto it = selection.rbegin(); it != selection.rend(); ++it)
            if (touches(*it))
                return *it;
        for (int i = numActive - 1; i >= 0; --i)
            if (touches(i))
                return i;
        return -1;
    }

    double clampOffset(int i, double offset) const
    {
        const double start = frameSeconds(analysis->notes[static_cast<size_t>(i)].startFrame);
        return clampSafe(offset, -start, duration - start - 0.05);
    }

    void seekTo(float x)
    {
        if (onSeek)
            onSeek(juce::jlimit(0.0, duration, xToTime(x)));
    }

    void selectionChanged()
    {
        if (onSelectionChanged)
            onSelectionChanged();
    }

    void commit()
    {
        for (auto it = edits.begin(); it != edits.end();)
            it = it->second.isIdentity() ? edits.erase(it) : std::next(it);
        repaint();
        if (onEditsChanged)
            onEditsChanged();
    }

    Shape noteShape(int i, float semis, double offset, float stretch = 1.0f) const
    {
        Shape s;
        const auto& n = analysis->notes[static_cast<size_t>(i)];
        polyphonic::NoteEdit timing;
        timing.stretch = stretch;
        const auto map = polyphonic::stretchMap(*analysis, i, timing);
        const auto& env = envelopes[static_cast<size_t>(i)];
        const int frames = static_cast<int>(n.f0.size());
        if (frames == 0)
            return s;
        const double hopSeconds = analysis->params.hop / analysis->params.sampleRate;
        const int stride = std::max(1, static_cast<int>(std::floor(2.0 / std::max(1e-9, hopSeconds * pxPerSecond))));
        const float maxHalf = 0.5f * noteThickness();
        std::vector<juce::Point<float>> top, bottom;
        for (int k = 0;; k = std::min(frames - 1, k + stride))
        {
            const float x = timeToX(analysis->frameToSeconds(map.outputFrame(n.startFrame + k)) + offset);
            const float y = midiToY(midiAt(i, k) + semis);
            const float level = k < static_cast<int>(env.size()) ? env[static_cast<size_t>(k)] : 1.0f;
            const float half = std::max(1.5f, maxHalf * (0.3f + 0.7f * level));
            top.push_back({ x, y - half });
            bottom.push_back({ x, y + half });
            if (s.curve.isEmpty())
                s.curve.startNewSubPath(x, y);
            else
                s.curve.lineTo(x, y);
            if (k == frames - 1)
                break;
        }
        if (top.size() == 1)
        {
            top.push_back(top[0].translated(3.0f, 0.0f));
            bottom.push_back(bottom[0].translated(3.0f, 0.0f));
        }
        s.body.startNewSubPath(top.front());
        for (size_t j = 1; j < top.size(); ++j)
            s.body.lineTo(top[j]);
        for (size_t j = bottom.size(); j-- > 0;)
            s.body.lineTo(bottom[j]);
        s.body.closeSubPath();
        s.body = s.body.createPathWithRoundedCorners(3.0f);
        s.bounds = s.body.getBounds();
        return s;
    }

    juce::Rectangle<float> noteBounds(int i) const
    {
        const auto e = editOf(i);
        return noteShape(i, e.semitones, e.timeOffsetSeconds, e.stretch).bounds;
    }

    bool visibleInClip(int i, const polyphonic::NoteEdit& e, const juce::Rectangle<float>& clip) const
    {
        const auto& n = analysis->notes[static_cast<size_t>(i)];
        const double t0 = frameSeconds(n.startFrame), t1 = frameSeconds(n.endFrame);
        const float xa = std::min(timeToX(t0), timeToX(t0 + e.timeOffsetSeconds)) - 6.0f;
        const float xb = std::max(timeToX(t1), timeToX(playedAt(i, e, n.endFrame))) + 6.0f;
        return xb >= clip.getX() && xa <= clip.getRight();
    }

    static void strokeDashed(juce::Graphics& g, const juce::Path& p, juce::Colour colour, float width)
    {
        juce::Path dashed;
        const float dashes[] = { 4.0f, 3.0f };
        juce::PathStrokeType(width).createDashedStroke(dashed, p, dashes, 2);
        g.setColour(colour);
        g.fillPath(dashed);
    }

    int noteAt(juce::Point<float> p) const
    {
        if (analysis == nullptr)
            return -1;
        const double framesPerSecond = analysis->params.sampleRate / analysis->params.hop;
        const auto hits = [&](int i) {
            const auto e = editOf(i);
            const auto& n = analysis->notes[static_cast<size_t>(i)];
            const auto map = polyphonic::stretchMap(*analysis, i, e);
            const double out = (xToTime(p.x) - e.timeOffsetSeconds) * framesPerSecond;
            const double slack = 3.0 / pxPerSecond * framesPerSecond;
            if (out < n.startFrame - slack || out > map.outputFrame(n.endFrame) + slack)
                return false;
            const double f = map.sourceFrame(out);
            const int k = juce::jlimit(0, static_cast<int>(n.f0.size()) - 1, static_cast<int>(std::lround(f)) - n.startFrame);
            const float y = midiToY(midiAt(i, k) + e.semitones);
            return std::abs(p.y - y) <= std::max(6.0f, 0.5f * noteThickness() + 2.0f);
        };
        for (auto it = selection.rbegin(); it != selection.rend(); ++it)
            if (hits(*it))
                return *it;
        for (int i = numActive - 1; i >= 0; --i)
            if (hits(i))
                return i;
        return -1;
    }

    double gridStep() const
    {
        static const double steps[] = { 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0 };
        for (double s : steps)
            if (s * pxPerSecond >= 70.0)
                return s;
        return 600.0;
    }

    void paintGrid(juce::Graphics& g, juce::Rectangle<float> area) const
    {
        const int mLo = static_cast<int>(std::floor(yToMidi(area.getBottom()))) - 1;
        const int mHi = static_cast<int>(std::ceil(yToMidi(area.getY()))) + 1;
        for (int m = mLo; m <= mHi; ++m)
        {
            const float y0 = midiToY(static_cast<float>(m) + 0.5f), y1 = midiToY(static_cast<float>(m) - 0.5f);
            if (isBlackKey(m))
            {
                g.setColour(kRowBlack);
                g.fillRect(area.getX(), y0, area.getWidth(), y1 - y0);
            }
            if (pitchClass(m) == 0)
            {
                g.setColour(APP_COLOR_BORDER);
                g.fillRect(area.getX(), y1 - 0.5f, area.getWidth(), 1.0f);
            }
        }
        const double step = gridStep();
        const double end = xToTime(area.getRight());
        g.setColour(APP_COLOR_BORDER_SUBTLE);
        for (auto k = static_cast<juce::int64>(std::ceil(viewStart / step));; ++k)
        {
            const double t = static_cast<double>(k) * step;
            if (t > end)
                break;
            g.fillRect(timeToX(t) - 0.5f, area.getY(), 1.0f, area.getHeight());
        }
    }

    void paintCandidate(juce::Graphics& g, int i, const juce::Rectangle<float>& clip) const
    {
        if (!visibleInClip(i, {}, clip))
            return;
        strokeDashed(g, noteShape(i, 0.0f, 0.0).body, APP_COLOR_ALERT_WARNING.withAlpha(0.5f), 1.0f);
    }

    void paintNote(juce::Graphics& g, int i, const juce::Rectangle<float>& clip) const
    {
        const auto e = editOf(i);
        if (!visibleInClip(i, e, clip))
            return;
        const bool selected = selection.count(i) > 0;
        const bool moved = e.semitones != 0.0f || e.timeOffsetSeconds != 0.0 || !e.pitchCurve.empty() || e.stretch != 1.0f;
        if (moved)
            strokeDashed(g, noteShape(i, 0.0f, 0.0).body, APP_COLOR_SECONDARY.withAlpha(0.6f), 1.2f);
        const auto s = noteShape(i, e.semitones, e.timeOffsetSeconds, e.stretch);
        if (e.mute)
        {
            strokeDashed(g, s.body, (selected ? APP_COLOR_PRIMARY_GLOW : APP_COLOR_TEXT_MUTED).withAlpha(0.9f), 1.2f);
            return;
        }
        juce::Colour top = APP_COLOR_NOTE_NORMAL;
        if (moved || e.gainDb != 0.0f)
            top = selected ? APP_COLOR_SECONDARY.brighter(0.3f) : APP_COLOR_SECONDARY;
        else if (selected)
            top = APP_COLOR_NOTE_SELECTED;
        else if (i == hovered)
            top = APP_COLOR_NOTE_HOVER;
        g.setGradientFill(juce::ColourGradient(top, 0.0f, s.bounds.getY(), top.darker(0.55f), 0.0f, s.bounds.getBottom(), false));
        g.fillPath(s.body);
        if (selected)
        {
            g.setColour(APP_COLOR_PRIMARY_GLOW);
            g.strokePath(s.body, juce::PathStrokeType(1.4f));
        }
        g.setColour((moved ? kEditInk : APP_COLOR_PITCH_CURVE).withAlpha(0.7f));
        g.strokePath(s.curve, juce::PathStrokeType(1.0f));
    }

    void paintHoverLabel(juce::Graphics& g, juce::Rectangle<float> area) const
    {
        if (hovered < 0 || drag != Drag::none)
            return;
        const auto e = editOf(hovered);
        const auto& n = analysis->notes[static_cast<size_t>(hovered)];
        const float midi = medians[static_cast<size_t>(hovered)];
        juce::String text = noteName(midi);
        if (e.semitones != 0.0f)
            text << " -> " << noteName(midi + e.semitones) << " (" << formatShift(e.semitones) << ")";
        text << "   " << formatTime(frameSeconds(n.startFrame) + e.timeOffsetSeconds, 2) << " - "
             << formatTime(playedAt(hovered, e, n.endFrame), 2);
        if (e.stretch != 1.0f)
            text << "   " << formatStretch(e.stretch);
        const auto font = AppFont::getFont(12.0f);
        g.setFont(font);
        const float w = juce::GlyphArrangement::getStringWidth(font, text) + 16.0f;
        const auto b = noteBounds(hovered);
        auto box = juce::Rectangle<float>(b.getX(), b.getY() - 26.0f, w, 20.0f);
        if (box.getY() < area.getY() + 2.0f)
            box.setY(b.getBottom() + 6.0f);
        box.setX(juce::jlimit(area.getX() + 2.0f, std::max(area.getX() + 2.0f, area.getRight() - w - 2.0f), box.getX()));
        g.setColour(APP_COLOR_SURFACE_RAISED.withAlpha(0.95f));
        g.fillRoundedRectangle(box, 5.0f);
        g.setColour(APP_COLOR_BORDER_HIGHLIGHT);
        g.drawRoundedRectangle(box, 5.0f, 1.0f);
        g.setColour(APP_COLOR_TEXT_PRIMARY);
        g.drawText(text, box.reduced(8.0f, 0.0f), juce::Justification::centredLeft, false);
    }

    void paintKeys(juce::Graphics& g) const
    {
        const auto keys = juce::Rectangle<float>(0.0f, static_cast<float>(kRulerHeight), static_cast<float>(kKeysWidth),
                                                 static_cast<float>(getHeight() - kRulerHeight));
        juce::Graphics::ScopedSaveState save(g);
        g.reduceClipRegion(keys.toNearestInt());
        g.setColour(APP_COLOR_SURFACE_ALT);
        g.fillRect(keys);
        const int mLo = static_cast<int>(std::floor(yToMidi(keys.getBottom()))) - 1;
        const int mHi = static_cast<int>(std::ceil(yToMidi(keys.getY()))) + 1;
        g.setFont(AppFont::getFont(10.5f));
        for (int m = mLo; m <= mHi; ++m)
        {
            const float y0 = midiToY(static_cast<float>(m) + 0.5f), y1 = midiToY(static_cast<float>(m) - 0.5f);
            g.setColour(isBlackKey(m) ? APP_COLOR_PIANO_BLACK : APP_COLOR_PIANO_WHITE);
            g.fillRect(keys.getX(), y0 + 0.5f, keys.getWidth() - 1.0f, std::max(0.5f, y1 - y0 - 1.0f));
            if (pitchClass(m) == 0 || pxPerSemitone >= 14.0f)
            {
                g.setColour(pitchClass(m) == 0 ? APP_COLOR_PIANO_TEXT : APP_COLOR_PIANO_TEXT_DIM);
                g.drawText(noteName(static_cast<float>(m)), juce::Rectangle<float>(4.0f, y0 - 4.0f, kKeysWidth - 10.0f, y1 - y0 + 8.0f),
                           juce::Justification::centredRight, false);
            }
        }
    }

    void paintRuler(juce::Graphics& g) const
    {
        const auto r = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(getWidth()), static_cast<float>(kRulerHeight));
        g.setColour(APP_COLOR_TIMELINE);
        g.fillRect(r);
        g.setColour(APP_COLOR_BORDER);
        g.fillRect(0.0f, r.getBottom() - 1.0f, r.getWidth(), 1.0f);
        juce::Graphics::ScopedSaveState save(g);
        g.reduceClipRegion(juce::Rectangle<int>(kKeysWidth, 0, getWidth() - kKeysWidth, kRulerHeight));
        const double step = gridStep();
        const int decimals = step >= 1.0 ? 0 : (step >= 0.1 && std::fmod(step * 10.0, 1.0) == 0.0 ? 1 : 2);
        const double end = xToTime(r.getRight());
        g.setFont(AppFont::getFont(10.5f));
        for (auto k = static_cast<juce::int64>(std::ceil(std::max(0.0, viewStart) / step));; ++k)
        {
            const double t = static_cast<double>(k) * step;
            if (t > end)
                break;
            const float x = timeToX(t);
            g.setColour(APP_COLOR_BORDER_HIGHLIGHT);
            g.fillRect(x - 0.5f, r.getBottom() - 7.0f, 1.0f, 6.0f);
            g.setColour(APP_COLOR_TEXT_MUTED);
            g.drawText(formatTime(t, decimals), juce::Rectangle<float>(x + 4.0f, 2.0f, 70.0f, kRulerHeight - 8.0f),
                       juce::Justification::centredLeft, false);
        }
        const float px = timeToX(playhead);
        juce::Path tri;
        tri.addTriangle(px - 5.0f, r.getBottom() - 9.0f, px + 5.0f, r.getBottom() - 9.0f, px, r.getBottom() - 1.0f);
        g.setColour(APP_COLOR_TEXT_PRIMARY);
        g.fillPath(tri);
    }

    std::map<int, polyphonic::NoteEdit>& edits;
    std::shared_ptr<const polyphonic::Analysis> analysis;
    std::vector<std::vector<float>> envelopes;
    std::vector<float> medians;
    int numActive = 0;
    double duration = 0.0;
    double playhead = 0.0;
    juce::String message;
    bool showCandidates = false;

    double viewStart = 0.0;
    double pxPerSecond = 100.0;
    float topMidi = 84.0f;
    float pxPerSemitone = 12.0f;
    bool autoFit = true;

    std::set<int> selection, bandBase;
    int hovered = -1;
    int pendingSingle = -1;
    Drag drag = Drag::none;
    juce::Point<float> dragStart;
    bool dragMoved = false;
    bool timeEngaged = false;
    std::map<int, polyphonic::NoteEdit> snapshot;
    juce::Rectangle<float> band;
    int stretchNote = -1;
};

bool writeWav(const juce::File& file, const Channels& channels, double sampleRate)
{
    if (channels.empty())
        return false;
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
    if (!static_cast<juce::FileOutputStream*>(stream.get())->openedOk())
        return false;
    juce::WavAudioFormat wav;
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate(sampleRate)
                             .withNumChannels(static_cast<int>(channels.size()))
                             .withBitsPerSample(24);
    const auto writer = wav.createWriterFor(stream, options);
    if (writer == nullptr)
        return false;
    std::vector<const float*> pointers;
    for (const auto& c : channels)
        pointers.push_back(c.data());
    return writer->writeFromFloatArrays(pointers.data(), static_cast<int>(pointers.size()), static_cast<int>(channels[0].size()));
}
} // namespace

//==============================================================================
class PolyLabWindow::Content : public juce::Component, private juce::Timer
{
public:
    Content(juce::AudioDeviceManager& dm, ProjectAudioProvider provider)
        : deviceManager(dm), projectAudio(std::move(provider)), canvas(edits)
    {
        evidenceModel = std::make_shared<EvidenceModel>();
        evidenceModel->file = PlatformPaths::getModelFile("basic_pitch_nmp.onnx");

        for (auto* b : { &projectButton, &openButton, &resetButton, &exportButton })
        {
            b->setColour(juce::TextButton::buttonColourId, APP_COLOR_SURFACE_RAISED);
            b->setColour(juce::TextButton::textColourOffId, APP_COLOR_TEXT_PRIMARY);
            b->setWantsKeyboardFocus(false);
            addAndMakeVisible(*b);
        }
        projectButton.setButtonText(TR("polylab.project_audio"));
        openButton.setButtonText(TR("polylab.open_audio"));
        resetButton.setButtonText(TR("polylab.reset"));
        exportButton.setButtonText(TR("polylab.export"));
        projectButton.onClick = [this] { loadProjectAudio(false); };
        openButton.onClick = [this] { openAudioFile(); };
        resetButton.onClick = [this] { resetEdits(); };
        exportButton.onClick = [this] { exportWav(); };

        const std::pair<juce::TextButton*, const char*> variants[] = {
            { &originalButton, "polylab.original" }, { &editedButton, "polylab.edited" }, { &soloButton, "polylab.solo" }
        };
        for (const auto& [b, key] : variants)
        {
            b->setButtonText(TR(key));
            b->setClickingTogglesState(true);
            b->setRadioGroupId(kVariantGroup);
            b->setColour(juce::TextButton::buttonColourId, APP_COLOR_SURFACE_RAISED);
            b->setColour(juce::TextButton::buttonOnColourId, APP_COLOR_PRIMARY);
            b->setColour(juce::TextButton::textColourOffId, APP_COLOR_TEXT_MUTED);
            b->setColour(juce::TextButton::textColourOnId, APP_COLOR_TEXT_PRIMARY);
            b->setColour(juce::ComboBox::outlineColourId, APP_COLOR_BORDER);
            b->setWantsKeyboardFocus(false);
            addAndMakeVisible(*b);
        }
        originalButton.setConnectedEdges(juce::Button::ConnectedOnRight);
        editedButton.setConnectedEdges(juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
        soloButton.setConnectedEdges(juce::Button::ConnectedOnLeft);
        originalButton.setToggleState(true, juce::dontSendNotification);
        originalButton.onClick = [this] { selectVariant(LabPlayer::original); };
        editedButton.onClick = [this] { selectVariant(LabPlayer::edited); };
        soloButton.onClick = [this] { selectVariant(LabPlayer::solo); };

        playButton.onClick = [this] { togglePlay(); };
        addAndMakeVisible(playButton);

        auditionButton.setToggleState(true, juce::dontSendNotification);
        auditionButton.setTooltip(TR("polylab.audition_tip"));
        auditionButton.onClick = [this] {
            if (!auditionButton.getToggleState())
                stopAudition();
            canvas.grabKeyboardFocus();
        };
        addAndMakeVisible(auditionButton);

        evidenceToggle.setButtonText(TR("polylab.evidence"));
        evidenceToggle.setToggleState(evidenceModel->file.existsAsFile(), juce::dontSendNotification);
        evidenceToggle.setWantsKeyboardFocus(false);
        evidenceToggle.onClick = [this] {
            if (source != nullptr)
                startAnalysis();
        };
        addAndMakeVisible(evidenceToggle);
        candidatesToggle.setButtonText(TR("polylab.show_candidates"));
        candidatesToggle.setWantsKeyboardFocus(false);
        candidatesToggle.onClick = [this] { canvas.setShowCandidates(candidatesToggle.getToggleState()); };
        addAndMakeVisible(candidatesToggle);

        for (auto* l : { &sourceLabel, &statusLabel, &selectionLabel, &timeLabel, &hintLabel })
        {
            l->setFont(AppFont::getFont(13.0f));
            l->setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED);
            l->setMinimumHorizontalScale(0.8f);
            l->setInterceptsMouseClicks(false, false);
            addAndMakeVisible(*l);
        }
        sourceLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
        selectionLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
        selectionLabel.setJustificationType(juce::Justification::centredRight);
        timeLabel.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain)));
        timeLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
        hintLabel.setFont(AppFont::getFont(12.0f));
        hintLabel.setText(TR("polylab.hint"), juce::dontSendNotification);

        canvas.onEditsChanging = [this] {
            updateSelectionLabel();
            auditionEditChanged();
        };
        canvas.onNotePressed = [this](int index) { startAudition(index); };
        canvas.onNoteReleased = [this] { stopAudition(); };
        canvas.onEditsChanged = [this] { editsChanged(); };
        canvas.onSelectionChanged = [this] { selectionChanged(); };
        canvas.onSeek = [this](double t) { player.seek(t); };
        canvas.onTogglePlay = [this] { togglePlay(); };
        canvas.onToggleCompare = [this] {
            selectVariant(player.getSelected() == LabPlayer::edited ? LabPlayer::original : LabPlayer::edited);
        };
        addAndMakeVisible(canvas);

        sourcePlayer.setSource(&player);
        deviceManager.addAudioCallback(&sourcePlayer);

        setSize(1200, 720);
        updateStatus();
        startTimerHz(30);
    }

    ~Content() override
    {
        stopTimer();
        for (const auto& flag : { analysisCancel, renderCancel, soloCancel, auditionCancel })
            if (flag != nullptr)
                flag->store(true);
        auditionPool.removeAllJobs(true, 60000);
        pool.removeAllJobs(true, 60000);
        deviceManager.removeAudioCallback(&sourcePlayer);
        sourcePlayer.setSource(nullptr);
    }

    void opened()
    {
        if (source == nullptr)
            loadProjectAudio(true);
        canvas.grabKeyboardFocus();
    }

    void stopPlayback() { player.pause(); }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(APP_COLOR_BACKGROUND);
        g.setColour(APP_COLOR_SURFACE);
        g.fillRect(0, 0, getWidth(), 2 * kBarHeight);
        g.setColour(APP_COLOR_BORDER_SUBTLE);
        g.fillRect(0, kBarHeight, getWidth(), 1);
        g.fillRect(0, 2 * kBarHeight, getWidth(), 1);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto top = r.removeFromTop(kBarHeight).reduced(14, 9);
        projectButton.setBounds(top.removeFromLeft(132));
        top.removeFromLeft(8);
        openButton.setBounds(top.removeFromLeft(132));
        top.removeFromLeft(16);
        candidatesToggle.setBounds(top.removeFromRight(196));
        top.removeFromRight(8);
        evidenceToggle.setBounds(top.removeFromRight(262));
        top.removeFromRight(12);
        sourceLabel.setBounds(top.withSizeKeepingCentre(top.getWidth(), 20));

        auto bar = r.removeFromTop(kBarHeight).reduced(14, 8);
        const auto oneLine = [](juce::Rectangle<int> b) { return b.withSizeKeepingCentre(b.getWidth(), 20); };
        playButton.setBounds(bar.removeFromLeft(34));
        bar.removeFromLeft(12);
        originalButton.setBounds(bar.removeFromLeft(92));
        editedButton.setBounds(bar.removeFromLeft(92));
        soloButton.setBounds(bar.removeFromLeft(72));
        bar.removeFromLeft(8);
        auditionButton.setBounds(bar.removeFromLeft(38));
        bar.removeFromLeft(14);
        timeLabel.setBounds(oneLine(bar.removeFromLeft(124)));
        exportButton.setBounds(bar.removeFromRight(124));
        bar.removeFromRight(8);
        resetButton.setBounds(bar.removeFromRight(116));
        bar.removeFromRight(14);
        selectionLabel.setBounds(oneLine(bar.removeFromRight(juce::jlimit(110, 280, bar.getWidth() * 2 / 5))));
        statusLabel.setBounds(oneLine(bar));

        hintLabel.setBounds(r.removeFromBottom(28).reduced(14, 4));
        canvas.setBounds(r.reduced(14, 0).withTrimmedTop(10));
    }

private:
    static constexpr int kBarHeight = 50;

    void timerCallback() override
    {
        const auto now = juce::Time::getMillisecondCounter();
        if (renderDueMs != 0 && now >= renderDueMs && !canvas.isDraggingNotes())
            startRender();
        if (soloDueMs != 0 && now >= soloDueMs)
            startSolo();
        const bool playing = player.isPlaying();
        canvas.setPlayhead(player.getPosition(), playing);
        playButton.setPlaying(playing);
        timeLabel.setText(formatTime(player.getPosition()) + " / " + formatTime(player.getLength()), juce::dontSendNotification);
        if (analysing)
            updateStatus();
    }

    void togglePlay()
    {
        if (player.isPlaying())
        {
            player.pause();
        }
        else
        {
            stopAudition();
            player.play();
        }
    }

    // ---- hearing the note under the mouse ----

    /** How a note sounds under its edit; where it sits in time does not change that. */
    polyphonic::NoteEdit soundOf(int index) const
    {
        const auto it = edits.find(index);
        auto e = it != edits.end() ? it->second : polyphonic::NoteEdit{};
        e.timeOffsetSeconds = 0.0;
        return e;
    }

    static bool sameSound(const polyphonic::NoteEdit& a, const polyphonic::NoteEdit& b)
    {
        return a.semitones == b.semitones && a.pitchCurve == b.pitchCurve && a.gainDb == b.gainDb && a.mute == b.mute
               && a.preserveFormants == b.preserveFormants && a.stretch == b.stretch;
    }

    void startAudition(int index)
    {
        stopAudition();
        if (!auditionButton.getToggleState() || player.isPlaying() || analysis == nullptr || source == nullptr
            || index < 0 || index >= static_cast<int>(analysis->notes.size()))
            return;
        auditionNote = index;
        auditionDirty = true;
        renderAudition();
    }

    void auditionEditChanged()
    {
        if (auditionNote >= 0 && !sameSound(soundOf(auditionNote), auditionLast))
        {
            auditionDirty = true;
            renderAudition();
        }
    }

    void stopAudition()
    {
        ++auditionGeneration;
        auditionNote = -1;
        auditionDirty = false;
        if (auditionCancel != nullptr)
            auditionCancel->store(true);
        player.stopAudition();
    }

    /** One render at a time: a change made while it runs is picked up when it finishes. */
    void renderAudition()
    {
        if (auditionBusy || !auditionDirty || auditionNote < 0 || analysis == nullptr || source == nullptr)
            return;
        auditionDirty = false;
        auditionBusy = true;
        const int index = auditionNote;
        const auto edit = soundOf(index);
        auditionLast = edit;
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        auditionCancel = cancel;
        const int generation = auditionGeneration;
        auto an = analysis;
        auto src = source;
        juce::Component::SafePointer<Content> safe(this);
        auditionPool.addJob([safe, an, src, index, edit, cancel, generation] {
            auto span = std::make_shared<polyphonic::AudioSpan>(
                polyphonic::renderNotes(*an, *src->channels, { index }, { { index, edit } }, cancel.get()));
            juce::MessageManager::callAsync([safe, span, index, generation] {
                if (safe != nullptr)
                    safe->auditionReady(generation, index, span);
            });
        });
    }

    void auditionReady(int generation, int index, std::shared_ptr<polyphonic::AudioSpan> span)
    {
        auditionBusy = false;
        if (generation == auditionGeneration && index == auditionNote && !player.isPlaying() && analysis != nullptr
            && !span->channels.empty() && !span->channels[0].empty())
        {
            // from just before the note starts
            const auto& n = analysis->notes[static_cast<size_t>(index)];
            const int onset = static_cast<int>(std::lround(analysis->frameToSeconds(n.startFrame) * source->sampleRate));
            const int from = std::max(span->start, onset - static_cast<int>(0.03 * source->sampleRate));
            const int start = span->start;
            player.audition(std::make_shared<const Channels>(std::move(span->channels)), start, from);
        }
        renderAudition();
    }

    void selectVariant(int slot)
    {
        player.select(slot);
        originalButton.setToggleState(slot == LabPlayer::original, juce::dontSendNotification);
        editedButton.setToggleState(slot == LabPlayer::edited, juce::dontSendNotification);
        soloButton.setToggleState(slot == LabPlayer::solo, juce::dontSendNotification);
        if (slot == LabPlayer::solo && !soloCurrent)
            soloDueMs = juce::Time::getMillisecondCounter();
        updateStatus();
    }

    void loadProjectAudio(bool quiet)
    {
        PolyLabSource src;
        if (!projectAudio || !projectAudio(src))
        {
            if (!quiet)
                setNote(TR("polylab.no_project_audio"));
            updateStatus();
            return;
        }
        loadSource(src);
    }

    void openAudioFile()
    {
        juce::Component::SafePointer<Content> safe(this);
        chooser = std::make_unique<juce::FileChooser>(TR("polylab.open_audio"), lastDirectory,
                                                      "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg;*.m4a");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [safe](const juce::FileChooser& fc) {
                                 if (safe == nullptr)
                                     return;
                                 const auto file = fc.getResult();
                                 if (file != juce::File())
                                     safe->loadFile(file);
                             });
    }

    void loadFile(const juce::File& file)
    {
        lastDirectory = file.getParentDirectory();
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        {
            setNote(TR("polylab.load_failed"));
            return;
        }
        if (static_cast<double>(reader->lengthInSamples) > kMaxSeconds * reader->sampleRate)
        {
            setNote(TR("polylab.too_long"));
            return;
        }
        PolyLabSource src;
        src.sampleRate = reader->sampleRate;
        src.name = file.getFileNameWithoutExtension();
        const int channels = static_cast<int>(std::min<unsigned int>(reader->numChannels, static_cast<unsigned int>(kMaxChannels)));
        const int length = static_cast<int>(reader->lengthInSamples);
        src.audio.setSize(std::max(1, channels), length);
        reader->read(&src.audio, 0, length, 0, true, channels > 1);
        loadSource(src);
    }

    void loadSource(const PolyLabSource& src)
    {
        if (src.audio.getNumSamples() == 0 || src.sampleRate <= 0.0)
        {
            setNote(TR("polylab.load_failed"));
            return;
        }
        if (src.audio.getNumSamples() > kMaxSeconds * src.sampleRate)
        {
            setNote(TR("polylab.too_long"));
            return;
        }
        player.pause();
        source = makeSource(src);
        player.setSource(source->channels, source->sampleRate);
        selectVariant(LabPlayer::original);
        canvas.setAnalysis(nullptr, source->seconds());
        const auto channels = source->channels->size() > 1 ? "stereo" : "mono";
        sourceLabel.setText(source->name + "   " + formatTime(source->seconds()) + "  ·  "
                                + juce::String(source->sampleRate / 1000.0, 1) + " kHz  ·  " + channels,
                            juce::dontSendNotification);
        note.clear();
        startAnalysis();
    }

    void startAnalysis()
    {
        if (source == nullptr)
            return;
        stopAudition();   // its note numbers belong to the old analysis
        for (const auto& flag : { analysisCancel, renderCancel, soloCancel })
            if (flag != nullptr)
                flag->store(true);
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        analysisCancel = cancel;
        analysis.reset();
        edits.clear();
        renderDueMs = soloDueMs = 0;
        rendering = soloing = false;
        soloCurrent = false;
        switchedAfterFirstEdit = false;
        canvas.setAnalysis(nullptr, source->seconds());
        player.setSlot(LabPlayer::edited, nullptr);
        player.setSlot(LabPlayer::solo, nullptr);
        selectVariant(LabPlayer::original);
        analysing = true;
        analysisProgress->store(0.0f);
        const int generation = ++analysisGeneration;
        const bool useEvidence = evidenceToggle.getToggleState();
        auto src = source;
        auto model = evidenceModel;
        auto progress = analysisProgress;
        juce::Component::SafePointer<Content> safe(this);
        pool.addJob([safe, src, model, progress, cancel, generation, useEvidence] {
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            polyphonic::NoteEvidence evidence;
            bool evidenceMissing = false;
            if (useEvidence)
            {
                if (model->ensureLoaded())
                    evidence = model->model.compute(src->mono.data(), src->numSamples(), src->sampleRate, cancel.get());
                else
                    evidenceMissing = true;
            }
            if (cancel->load())
                return;
            progress->store(0.15f);
            const auto params = polyphonic::Params::forSampleRate(src->sampleRate);
            auto result = std::make_shared<polyphonic::Analysis>(polyphonic::analyze(
                src->mono.data(), src->numSamples(), params, evidence.empty() ? nullptr : &evidence, cancel.get(),
                [progress](float v) { progress->store(0.15f + 0.85f * v); }));
            if (cancel->load())
                return;
            const double seconds = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
            const bool usedEvidence = !evidence.empty();
            juce::MessageManager::callAsync([safe, result, generation, seconds, usedEvidence, evidenceMissing] {
                if (safe != nullptr)
                    safe->analysisFinished(result, generation, seconds, usedEvidence, evidenceMissing);
            });
        });
        updateStatus();
    }

    void analysisFinished(std::shared_ptr<const polyphonic::Analysis> result, int generation, double seconds,
                          bool usedEvidence, bool evidenceMissing)
    {
        if (generation != analysisGeneration)
            return;
        analysing = false;
        analysis = std::move(result);
        analysisSeconds = seconds;
        analysedWithEvidence = usedEvidence;
        if (evidenceMissing)
            setNote(TR("polylab.evidence_missing"));
        edits.clear();
        canvas.setAnalysis(analysis, source->seconds());
        player.setSlot(LabPlayer::edited, source->channels);   // identical until something is edited
        updateStatus();
        canvas.grabKeyboardFocus();
    }

    void editsChanged()
    {
        renderDueMs = juce::Time::getMillisecondCounter() + 40;
        if (player.getSelected() == LabPlayer::original && !edits.empty() && !switchedAfterFirstEdit)
        {
            switchedAfterFirstEdit = true;   // the first edit switches to the result once
            selectVariant(LabPlayer::edited);
        }
        updateStatus();
    }

    void startRender()
    {
        renderDueMs = 0;
        if (analysis == nullptr || source == nullptr)
            return;
        if (renderCancel != nullptr)
            renderCancel->store(true);
        if (edits.empty())
        {
            rendering = false;
            player.setSlot(LabPlayer::edited, source->channels);
            updateStatus();
            return;
        }
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        renderCancel = cancel;
        rendering = true;
        const int generation = ++renderGeneration;
        auto an = analysis;
        auto src = source;
        auto toRender = edits;
        juce::Component::SafePointer<Content> safe(this);
        pool.addJob([safe, an, src, toRender, cancel, generation] {
            auto out = std::make_shared<Channels>(polyphonic::render(*an, *src->channels, toRender, cancel.get()));
            if (cancel->load() || out->empty())
                return;
            juce::MessageManager::callAsync([safe, out, generation] {
                if (safe != nullptr)
                    safe->renderFinished(generation, out);
            });
        });
        updateStatus();
    }

    void renderFinished(int generation, std::shared_ptr<const Channels> out)
    {
        if (generation != renderGeneration)
            return;
        rendering = false;
        player.setSlot(LabPlayer::edited, std::move(out));
        updateStatus();
    }

    void selectionChanged()
    {
        soloCurrent = false;
        if (player.getSelected() == LabPlayer::solo)
            soloDueMs = juce::Time::getMillisecondCounter() + 120;
        updateSelectionLabel();
    }

    void startSolo()
    {
        soloDueMs = 0;
        if (analysis == nullptr || source == nullptr)
            return;
        if (soloCancel != nullptr)
            soloCancel->store(true);
        const auto& sel = canvas.getSelection();
        std::vector<int> notes(sel.begin(), sel.end());
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        soloCancel = cancel;
        soloing = true;
        soloCurrent = true;
        const int generation = ++soloGeneration;
        auto an = analysis;
        auto src = source;
        juce::Component::SafePointer<Content> safe(this);
        pool.addJob([safe, an, src, notes, cancel, generation] {
            auto out = std::make_shared<Channels>(polyphonic::extractNotes(*an, *src->channels, notes, cancel.get()));
            if (cancel->load() || out->empty())
                return;
            juce::MessageManager::callAsync([safe, out, generation] {
                if (safe != nullptr)
                    safe->soloFinished(generation, out);
            });
        });
        updateStatus();
    }

    void soloFinished(int generation, std::shared_ptr<const Channels> out)
    {
        if (generation != soloGeneration)
            return;
        soloing = false;
        player.setSlot(LabPlayer::solo, std::move(out));
        updateStatus();
    }

    void resetEdits()
    {
        if (edits.empty())
            return;
        edits.clear();
        canvas.repaint();
        editsChanged();
    }

    void exportWav()
    {
        if (source == nullptr || analysis == nullptr)
            return;
        if (rendering || renderDueMs != 0)
        {
            setNote(TR("polylab.render_wait"));
            return;
        }
        auto audio = edits.empty() ? source->channels : player.getSlot(LabPlayer::edited);
        if (audio == nullptr)
            return;
        const double rate = source->sampleRate;
        const auto folder = lastDirectory.isDirectory() ? lastDirectory
                                                        : juce::File::getSpecialLocation(juce::File::userMusicDirectory);
        juce::Component::SafePointer<Content> safe(this);
        chooser = std::make_unique<juce::FileChooser>(TR("polylab.export"), folder.getChildFile(source->name + " (edited).wav"), "*.wav");
        chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::warnAboutOverwriting,
                             [safe, audio, rate](const juce::FileChooser& fc) {
                                 auto file = fc.getResult();
                                 if (safe == nullptr || file == juce::File())
                                     return;
                                 file = file.withFileExtension("wav");
                                 safe->lastDirectory = file.getParentDirectory();
                                 const bool ok = writeWav(file, *audio, rate);
                                 safe->setNote(ok ? TR("polylab.exported") + " " + file.getFileName() : TR("polylab.export_failed"));
                             });
    }

    void setNote(const juce::String& text)
    {
        note = text;
        updateStatus();
    }

    void updateStatus()
    {
        juce::String text;
        if (source == nullptr)
        {
            canvas.setMessage(TR("polylab.no_audio"));
        }
        else if (analysing)
        {
            const int percent = juce::roundToInt(analysisProgress->load() * 100.0f);
            text = TR("polylab.analysing") + "... " + juce::String(percent) + "%";
            canvas.setMessage(text);
        }
        else if (analysis != nullptr)
        {
            int active = 0;
            for (const auto& n : analysis->notes)
                active += n.active ? 1 : 0;
            canvas.setMessage(active == 0 ? TR("polylab.no_notes") : juce::String());
            text = juce::String(active) + " " + TR("polylab.notes") + "  ·  " + TR("polylab.analysed") + " "
                   + juce::String(analysisSeconds, 1) + " s";
            if (!analysedWithEvidence)
                text << "  ·  " << TR("polylab.dsp_only");
            if (rendering || renderDueMs != 0 || soloing)
                text << "  ·  " << TR("polylab.rendering") << "...";
        }
        if (note.isNotEmpty())
            text << (text.isEmpty() ? "" : "  ·  ") << note;
        statusLabel.setText(text, juce::dontSendNotification);
        if (player.getSelected() == LabPlayer::solo && canvas.getSelection().empty() && analysis != nullptr)
            statusLabel.setText(TR("polylab.solo_empty"), juce::dontSendNotification);
        resetButton.setEnabled(!edits.empty());
        exportButton.setEnabled(analysis != nullptr);
        playButton.setEnabled(source != nullptr);
        updateSelectionLabel();
    }

    void updateSelectionLabel()
    {
        juce::String text;
        const auto& sel = canvas.getSelection();
        if (analysis != nullptr && sel.size() == 1)
        {
            const int i = *sel.begin();
            const auto it = edits.find(i);
            const auto e = it != edits.end() ? it->second : polyphonic::NoteEdit{};
            const float midi = analysis->notes[static_cast<size_t>(i)].medianMidi();
            text = noteName(midi);
            if (e.semitones != 0.0f)
                text << " -> " << noteName(midi + e.semitones) << "  (" << formatShift(e.semitones) << ")";
            if (e.timeOffsetSeconds != 0.0)
                text << "  " << (e.timeOffsetSeconds > 0.0 ? "+" : "") << juce::String(e.timeOffsetSeconds, 2) << " s";
            if (e.stretch != 1.0f)
                text << "  " << formatStretch(e.stretch);
            if (e.mute)
                text << "  " << TR("polylab.muted");
        }
        else if (sel.size() > 1)
        {
            text = juce::String(static_cast<int>(sel.size())) + " " + TR("polylab.notes_selected");
        }
        else if (!edits.empty())
        {
            text = juce::String(static_cast<int>(edits.size())) + " " + TR("polylab.notes_edited");
        }
        selectionLabel.setText(text, juce::dontSendNotification);
    }

    juce::AudioDeviceManager& deviceManager;
    ProjectAudioProvider projectAudio;
    LabPlayer player;
    juce::AudioSourcePlayer sourcePlayer;

    std::map<int, polyphonic::NoteEdit> edits;
    NoteCanvas canvas;
    juce::TextButton projectButton, openButton, resetButton, exportButton;
    juce::TextButton originalButton, editedButton, soloButton;
    PlayButton playButton;
    AuditionButton auditionButton;
    juce::TooltipWindow tooltips { this, 600 };
    juce::ToggleButton evidenceToggle, candidatesToggle;
    juce::Label sourceLabel, statusLabel, selectionLabel, timeLabel, hintLabel;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::File lastDirectory;

    std::shared_ptr<EvidenceModel> evidenceModel;
    std::shared_ptr<const SourceData> source;
    std::shared_ptr<const polyphonic::Analysis> analysis;
    std::shared_ptr<std::atomic<bool>> analysisCancel, renderCancel, soloCancel;
    std::shared_ptr<std::atomic<float>> analysisProgress = std::make_shared<std::atomic<float>>(0.0f);
    int analysisGeneration = 0;
    int renderGeneration = 0;
    int soloGeneration = 0;
    bool analysing = false;
    bool rendering = false;
    bool soloing = false;
    bool soloCurrent = false;
    bool analysedWithEvidence = false;
    bool switchedAfterFirstEdit = false;
    double analysisSeconds = 0.0;
    juce::uint32 renderDueMs = 0;
    juce::uint32 soloDueMs = 0;
    juce::String note;

    // the note under the mouse, heard on its own
    int auditionNote = -1;
    int auditionGeneration = 0;
    bool auditionBusy = false;    // a render is running
    bool auditionDirty = false;   // the note's sound changed since that render started
    polyphonic::NoteEdit auditionLast;
    std::shared_ptr<std::atomic<bool>> auditionCancel;

    // Declared last: jobs hold only shared copies, but they post back to this object. Hearing a
    // note has its own thread so it never waits behind a full render.
    juce::ThreadPool pool { 1 };
    juce::ThreadPool auditionPool { 1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Content)
};

//==============================================================================
PolyLabWindow::PolyLabWindow(juce::AudioDeviceManager& deviceManager, ProjectAudioProvider projectAudio)
    : juce::DocumentWindow(TR("polylab.title"), APP_COLOR_BACKGROUND, juce::DocumentWindow::allButtons)
{
    setLookAndFeel(&DarkLookAndFeel::getInstance());
    setUsingNativeTitleBar(true);
    content = std::make_unique<Content>(deviceManager, std::move(projectAudio));
    setContentNonOwned(content.get(), true);
    setResizable(true, false);
    setResizeLimits(900, 520, 10000, 10000);
    centreWithSize(getWidth(), getHeight());
}

PolyLabWindow::~PolyLabWindow()
{
    clearContentComponent();
    content.reset();
    setLookAndFeel(nullptr);
}

void PolyLabWindow::open()
{
    setVisible(true);
    toFront(true);
    content->opened();
}

void PolyLabWindow::closeButtonPressed()
{
    content->stopPlayback();
    setVisible(false);
}
