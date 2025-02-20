/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore BVBA and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "playbackcontroller.h"

#include "log.h"

#include "playbacktypes.h"

using namespace mu::playback;
using namespace mu::midi;
using namespace mu::notation;
using namespace mu::async;
using namespace mu::audio;
using namespace mu::actions;
using namespace mu::engraving;

static const ActionCode PLAY_CODE("play");
static const ActionCode STOP_CODE("stop");
static const ActionCode REWIND_CODE("rewind");
static const ActionCode LOOP_CODE("loop");
static const ActionCode LOOP_IN_CODE("loop-in");
static const ActionCode LOOP_OUT_CODE("loop-out");
static const ActionCode METRONOME_CODE("metronome");
static const ActionCode MIDI_ON_CODE("midi-on");
static const ActionCode COUNT_IN_CODE("countin");
static const ActionCode PAN_CODE("pan");
static const ActionCode REPEAT_CODE("repeat");

void PlaybackController::init()
{
    dispatcher()->reg(this, PLAY_CODE, this, &PlaybackController::togglePlay);
    dispatcher()->reg(this, STOP_CODE, this, &PlaybackController::pause);
    dispatcher()->reg(this, REWIND_CODE, this, &PlaybackController::rewind);
    dispatcher()->reg(this, LOOP_CODE, this, &PlaybackController::toggleLoopPlayback);
    dispatcher()->reg(this, LOOP_IN_CODE, [this]() { addLoopBoundary(LoopBoundaryType::LoopIn); });
    dispatcher()->reg(this, LOOP_OUT_CODE, [this]() { addLoopBoundary(LoopBoundaryType::LoopOut); });
    dispatcher()->reg(this, REPEAT_CODE, this, &PlaybackController::togglePlayRepeats);
    dispatcher()->reg(this, PAN_CODE, this, &PlaybackController::toggleAutomaticallyPan);
    dispatcher()->reg(this, METRONOME_CODE, this, &PlaybackController::toggleMetronome);
    dispatcher()->reg(this, MIDI_ON_CODE, this, &PlaybackController::toggleMidiInput);
    dispatcher()->reg(this, COUNT_IN_CODE, this, &PlaybackController::toggleCountIn);

    globalContext()->currentNotationChanged().onNotify(this, [this]() {
        onNotationChanged();
    });

    globalContext()->currentProjectChanged().onNotify(this, [this]() {
        if (m_currentSequenceId != -1) {
            resetCurrentSequence();
        }

        playback()->addSequence().onResolve(this, [this](const TrackSequenceId& sequenceId) {
            setupNewCurrentSequence(sequenceId);
        });
    });

    m_needRewindBeforePlay = true;
}

int PlaybackController::currentTick() const
{
    return m_currentTick;
}

bool PlaybackController::isPlayAllowed() const
{
    return m_notation != nullptr && !m_notation->interaction()->isElementEditStarted();
}

Notification PlaybackController::isPlayAllowedChanged() const
{
    return m_isPlayAllowedChanged;
}

bool PlaybackController::isPlaying() const
{
    return m_isPlaying;
}

bool PlaybackController::isPaused() const
{
    return !m_isPlaying;
}

bool PlaybackController::isLoopVisible() const
{
    return notationPlayback() ? notationPlayback()->loopBoundaries().val.visible : false;
}

bool PlaybackController::isPlaybackLooped() const
{
    return notationPlayback() ? !notationPlayback()->loopBoundaries().val.isNull() : false;
}

Notification PlaybackController::isPlayingChanged() const
{
    return m_isPlayingChanged;
}

void PlaybackController::reset()
{
    stop();
}

void PlaybackController::seek(const midi::tick_t tick)
{
    IF_ASSERT_FAILED(notationPlayback() && playback()) {
        return;
    }

    msecs_t milliseconds = secondsToMilliseconds(notationPlayback()->playedTickToSec(tick));
    playback()->player()->seek(m_currentSequenceId, std::move(milliseconds));
}

void PlaybackController::seek(const audio::msecs_t msecs)
{
    IF_ASSERT_FAILED(notationPlayback() && playback()) {
        return;
    }

    playback()->player()->seek(m_currentSequenceId, msecs);
}

Notification PlaybackController::playbackPositionChanged() const
{
    return m_playbackPositionChanged;
}

Channel<uint32_t> PlaybackController::midiTickPlayed() const
{
    return m_tickPlayed;
}

float PlaybackController::playbackPositionInSeconds() const
{
    return notationPlayback() ? notationPlayback()->playedTickToSec(m_currentTick) : 0.0;
}

TrackSequenceId PlaybackController::currentTrackSequenceId() const
{
    return m_currentSequenceId;
}

Notification PlaybackController::currentTrackSequenceIdChanged() const
{
    return m_currentSequenceIdChanged;
}

void PlaybackController::playElement(const notation::EngravingItem* element)
{
    IF_ASSERT_FAILED(element) {
        return;
    }

    IF_ASSERT_FAILED(notationPlayback()) {
        return;
    }

    RetVal<midi::tick_t> tick = notationPlayback()->playPositionTickByElement(element);
    if (!tick.ret) {
        return;
    }

    seek(tick.val);

    if (!configuration()->playNotesWhenEditing()) {
        return;
    }

    notationPlayback()->triggerEventsForItem(element);
}

INotationPlaybackPtr PlaybackController::notationPlayback() const
{
    return m_masterNotation ? m_masterNotation->playback() : nullptr;
}

INotationPartsPtr PlaybackController::masterNotationParts() const
{
    return m_masterNotation ? m_masterNotation->parts() : nullptr;
}

INotationSelectionPtr PlaybackController::selection() const
{
    return m_notation ? m_notation->interaction()->selection() : nullptr;
}

void PlaybackController::onNotationChanged()
{
    m_masterNotation = globalContext()->currentMasterNotation();
    m_notation = globalContext()->currentNotation();
    if (!m_notation || !m_masterNotation) {
        m_isPlayAllowedChanged.notify();
        return;
    }

    INotationPartsPtr notationParts = m_notation->parts();

    if (!m_trackIdMap.empty()) {
        for (const Part* part : m_masterNotation->parts()->partList()) {
            bool isActive = notationParts->partExists(part->id());

            for (const InstrumentTrackId& instrumentTrackId : part->instrumentTrackIdSet()) {
                setTrackActivity(instrumentTrackId, isActive);
            }
        }
    }

    notationPlayback()->loopBoundaries().ch.onReceive(this, [this](const LoopBoundaries& boundaries) {
        setLoop(boundaries);
    });

    m_isPlayAllowedChanged.notify();
}

void PlaybackController::togglePlay()
{
    if (!isPlayAllowed()) {
        LOGW() << "playback not allowed";
        return;
    }

    if (isPlaying()) {
        pause();
    } else if (isPaused()) {
        resume();
    } else {
        play();
    }
}

void PlaybackController::play()
{
    IF_ASSERT_FAILED(notationPlayback() && playback()) {
        return;
    }

    if (m_needRewindBeforePlay) {
        seek(m_currentTick);
    } else {
        m_needRewindBeforePlay = true;
    }

    playback()->player()->play(m_currentSequenceId);

    m_isPlaying = true;
    m_isPlayingChanged.notify();
}

void PlaybackController::rewind(const ActionData& args)
{
    IF_ASSERT_FAILED(playback()) {
        return;
    }

    msecs_t newPosition = 0;

    if (!args.empty()) {
        newPosition = args.arg<msecs_t>(0);
    }

    if (m_currentPlaybackStatus == PlaybackStatus::Running) {
        seek(newPosition);
        m_needRewindBeforePlay = false;
    } else {
        stop();
    }
}

void PlaybackController::pause()
{
    IF_ASSERT_FAILED(playback()) {
        return;
    }

    playback()->player()->pause(m_currentSequenceId);

    m_isPlaying = false;
    m_isPlayingChanged.notify();
}

void PlaybackController::stop()
{
    IF_ASSERT_FAILED(playback()) {
        return;
    }

    playback()->player()->stop(m_currentSequenceId);
    m_isPlaying = false;
    m_isPlayingChanged.notify();
}

void PlaybackController::resume()
{
    IF_ASSERT_FAILED(playback()) {
        return;
    }

    playback()->player()->resume(m_currentSequenceId);

    m_isPlaying = true;
    m_isPlayingChanged.notify();
}

void PlaybackController::togglePlayRepeats()
{
    bool playRepeatsEnabled = notationConfiguration()->isPlayRepeatsEnabled();
    notationConfiguration()->setIsPlayRepeatsEnabled(!playRepeatsEnabled);
    notifyActionCheckedChanged(REPEAT_CODE);
}

void PlaybackController::toggleAutomaticallyPan()
{
    bool panEnabled = notationConfiguration()->isAutomaticallyPanEnabled();
    notationConfiguration()->setIsAutomaticallyPanEnabled(!panEnabled);
    notifyActionCheckedChanged(PAN_CODE);
}

void PlaybackController::toggleMetronome()
{
    bool metronomeEnabled = notationConfiguration()->isMetronomeEnabled();
    notationConfiguration()->setIsMetronomeEnabled(!metronomeEnabled);
    notifyActionCheckedChanged(METRONOME_CODE);

    setTrackActivity(notationPlayback()->metronomeTrackId(), !metronomeEnabled);
}

void PlaybackController::toggleMidiInput()
{
    bool midiInputEnabled = notationConfiguration()->isMidiInputEnabled();
    notationConfiguration()->setIsMidiInputEnabled(!midiInputEnabled);
    notifyActionCheckedChanged(MIDI_ON_CODE);
}

void PlaybackController::toggleCountIn()
{
    bool countInEnabled = notationConfiguration()->isCountInEnabled();
    notationConfiguration()->setIsCountInEnabled(!countInEnabled);
    notifyActionCheckedChanged(COUNT_IN_CODE);
}

void PlaybackController::toggleLoopPlayback()
{
    if (isLoopVisible()) {
        hideLoop();
        return;
    }

    if (isPlaybackLooped() && !selection()->isRange()) {
        showLoop();
        return;
    }

    int loopInTick = 0;
    int loopOutTick = 0;

    if (!selection()->isNone()) {
        loopInTick = selection()->range()->startTick().ticks();
        loopOutTick = selection()->range()->endTick().ticks();
    }

    if (loopInTick <= 0) {
        loopInTick = INotationPlayback::FirstScoreTick;
    }

    if (loopOutTick <= 0) {
        loopOutTick = INotationPlayback::LastScoreTick;
    }

    addLoopBoundaryToTick(LoopBoundaryType::LoopIn, loopInTick);
    addLoopBoundaryToTick(LoopBoundaryType::LoopOut, loopOutTick);
}

void PlaybackController::addLoopBoundary(LoopBoundaryType type)
{
    if (isPlaying()) {
        addLoopBoundaryToTick(type, currentTick());
    } else {
        addLoopBoundaryToTick(type, INotationPlayback::SelectedNoteTick);
    }
}

void PlaybackController::addLoopBoundaryToTick(LoopBoundaryType type, int tick)
{
    if (notationPlayback()) {
        notationPlayback()->addLoopBoundary(type, tick);
        showLoop();
    }
}

void PlaybackController::setLoop(const LoopBoundaries& boundaries)
{
    IF_ASSERT_FAILED(playback()) {
        return;
    }

    if (!boundaries.visible) {
        hideLoop();
        return;
    }

    msecs_t fromMilliseconds = secondsToMilliseconds(notationPlayback()->playedTickToSec(boundaries.loopInTick));
    msecs_t toMilliseconds = secondsToMilliseconds(notationPlayback()->playedTickToSec(boundaries.loopOutTick));
    playback()->player()->setLoop(m_currentSequenceId, fromMilliseconds, toMilliseconds);
    showLoop();

    notifyActionCheckedChanged(LOOP_CODE);
}

void PlaybackController::showLoop()
{
    if (notationPlayback()) {
        notationPlayback()->setLoopBoundariesVisible(true);
    }
}

void PlaybackController::hideLoop()
{
    IF_ASSERT_FAILED(notationPlayback() && playback()) {
        return;
    }

    playback()->player()->resetLoop(m_currentSequenceId);
    notationPlayback()->setLoopBoundariesVisible(false);
    notifyActionCheckedChanged(LOOP_CODE);
}

void PlaybackController::notifyActionCheckedChanged(const ActionCode& actionCode)
{
    m_actionCheckedChanged.send(actionCode);
}

mu::project::IProjectAudioSettingsPtr PlaybackController::audioSettings() const
{
    IF_ASSERT_FAILED(globalContext()->currentProject()) {
        return nullptr;
    }

    return globalContext()->currentProject()->audioSettings();
}

void PlaybackController::resetCurrentSequence()
{
    playback()->player()->playbackPositionMsecs().resetOnReceive(this);
    playback()->player()->playbackStatusChanged().resetOnReceive(this);

    playback()->tracks()->inputParamsChanged().resetOnReceive(this);
    playback()->audioOutput()->outputParamsChanged().resetOnReceive(this);
    playback()->audioOutput()->masterOutputParamsChanged().resetOnReceive(this);

    setCurrentTick(0);
    m_isPlaying = false;
    m_isPlayingChanged.notify();

    playback()->removeSequence(m_currentSequenceId);
    m_currentSequenceId = -1;
}

void PlaybackController::setCurrentTick(const tick_t tick)
{
    m_currentTick = tick;
    m_playbackPositionChanged.notify();
}

void PlaybackController::addTrack(const InstrumentTrackId& instrumentTrackId, const std::string& title)
{
    IF_ASSERT_FAILED(notationPlayback() && playback()) {
        return;
    }

    if (!instrumentTrackId.isValid()) {
        return;
    }

    AudioInputParams inParams = audioSettings()->trackInputParams(instrumentTrackId);
    AudioOutputParams outParams = trackOutputParams(instrumentTrackId);
    mpe::PlaybackData playbackData = notationPlayback()->trackPlaybackData(instrumentTrackId);

    if (!playbackData.isValid()) {
        return;
    }

    uint64_t notationPlaybackKey = reinterpret_cast<uint64_t>(notationPlayback().get());

    playback()->tracks()->addTrack(m_currentSequenceId, title, std::move(playbackData), { std::move(inParams), std::move(outParams) })
    .onResolve(this, [this, instrumentTrackId, notationPlaybackKey](const TrackId trackId, const AudioParams& appliedParams) {
        //! NOTE It may be that while we were adding a track, the notation was already closed (or opened another)
        //! This situation can be if the notation was opened and immediately closed.
        quint64 currentNotationPlaybackKey = reinterpret_cast<uint64_t>(notationPlayback().get());
        if (currentNotationPlaybackKey != notationPlaybackKey) {
            return;
        }

        m_trackIdMap.insert({ instrumentTrackId, trackId });

        audioSettings()->setTrackInputParams(instrumentTrackId, appliedParams.in);
        audioSettings()->setTrackOutputParams(instrumentTrackId, appliedParams.out);
    })
    .onReject(this, [](int code, const std::string& msg) {
        LOGE() << "can't add a new track, code: [" << code << "] " << msg;
    });
}

void PlaybackController::setTrackActivity(const engraving::InstrumentTrackId& instrumentTrackId, const bool isActive)
{
    IF_ASSERT_FAILED(audioSettings() && playback()) {
        return;
    }

    AudioOutputParams outParams = audioSettings()->trackOutputParams(instrumentTrackId);

    outParams.muted = !isActive;

    audio::TrackId trackId = m_trackIdMap[instrumentTrackId];
    playback()->audioOutput()->setOutputParams(m_currentSequenceId, trackId, std::move(outParams));
}

AudioOutputParams PlaybackController::trackOutputParams(const engraving::InstrumentTrackId& instrumentTrackId) const
{
    IF_ASSERT_FAILED(audioSettings() && notationConfiguration() && notationPlayback()) {
        return {};
    }

    AudioOutputParams result = audioSettings()->trackOutputParams(instrumentTrackId);

    if (instrumentTrackId == notationPlayback()->metronomeTrackId()) {
        result.muted = !notationConfiguration()->isMetronomeEnabled();
    }

    return result;
}

void PlaybackController::removeNonExistingTracks()
{
    for (const auto& pair : m_trackIdMap) {
        if (!masterNotationParts()->partExists(pair.first.partId)) {
            removeTrack(pair.first);
            continue;
        }

        const Part* part = masterNotationParts()->part(pair.first.partId);
        const InstrumentTrackIdSet& idSet = part->instrumentTrackIdSet();

        if (idSet.find(pair.first) == idSet.cend()) {
            removeTrack(pair.first);
        }
    }
}

void PlaybackController::removeTrack(const InstrumentTrackId& partId)
{
    IF_ASSERT_FAILED(notationPlayback() && playback()) {
        return;
    }

    auto search = m_trackIdMap.find(partId);

    if (search == m_trackIdMap.end()) {
        return;
    }

    playback()->tracks()->removeTrack(m_currentSequenceId, search->second);
    audioSettings()->removeTrackParams(partId);
}

void PlaybackController::setupNewCurrentSequence(const TrackSequenceId sequenceId)
{
    playback()->tracks()->removeAllTracks(m_currentSequenceId);

    m_currentSequenceId = sequenceId;
    m_currentSequenceIdChanged.notify();

    if (!notationPlayback()) {
        return;
    }

    audio::AudioOutputParams masterOutputParams = audioSettings()->masterAudioOutputParams();
    playback()->audioOutput()->setMasterOutputParams(masterOutputParams);

    subscribeOnAudioParamsChanges();
    setupSequenceTracks();
    setupSequencePlayer();
}

void PlaybackController::subscribeOnAudioParamsChanges()
{
    playback()->audioOutput()->masterOutputParamsChanged().onReceive(this, [this](const audio::AudioOutputParams& params) {
        audioSettings()->setMasterAudioOutputParams(params);
    });

    playback()->tracks()->inputParamsChanged().onReceive(this,
                                                         [this](const TrackSequenceId sequenceId,
                                                                const TrackId trackId,
                                                                const AudioInputParams& params) {
        if (sequenceId != m_currentSequenceId) {
            return;
        }

        auto search = std::find_if(m_trackIdMap.begin(), m_trackIdMap.end(), [trackId](const auto& pair) {
            return pair.second == trackId;
        });

        if (search != m_trackIdMap.end()) {
            audioSettings()->setTrackInputParams(search->first, params);
        }
    });

    playback()->audioOutput()->outputParamsChanged().onReceive(this,
                                                               [this](const TrackSequenceId sequenceId,
                                                                      const TrackId trackId,
                                                                      const AudioOutputParams& params) {
        if (sequenceId != m_currentSequenceId) {
            return;
        }

        auto search = std::find_if(m_trackIdMap.begin(), m_trackIdMap.end(), [trackId](const auto& pair) {
            return pair.second == trackId;
        });

        if (search != m_trackIdMap.end()) {
            audioSettings()->setTrackOutputParams(search->first, params);
        }
    });
}

void PlaybackController::setupSequenceTracks()
{
    m_trackIdMap.clear();

    if (!masterNotationParts()) {
        return;
    }

    NotifyList<const Part*> partList = masterNotationParts()->partList();

    for (const Part* part : partList) {
        for (const auto& pair : *part->instruments()) {
            addTrack({ part->id(), pair.second->id().toStdString() }, part->partName().toStdString());
        }
    }

    addTrack(notationPlayback()->metronomeTrackId(), qtrc("playback", "Metronome").toStdString());

    partList.onItemAdded(this, [this](const Part* part) {
        for (const auto& pair : *part->instruments()) {
            addTrack({ part->id(), pair.second->id().toStdString() }, part->partName().toStdString());
        }
    });

    partList.onItemChanged(this, [this](const Part* part) {
        for (const auto& pair : *part->instruments()) {
            InstrumentTrackId trackId = { part->id(), pair.second->id().toStdString() };
            AudioOutputParams params = trackOutputParams(trackId);

            auto search = m_trackIdMap.find(trackId);
            if (search == m_trackIdMap.cend()) {
                addTrack(trackId, part->partName().toStdString());
                removeNonExistingTracks();
                continue;
            }

            if (params.muted == !part->isVisible()) {
                continue;
            }

            setTrackActivity(trackId, part->isVisible());
        }
    });

    partList.onItemRemoved(this, [this](const Part* part) {
        for (const auto& pair : *part->instruments()) {
            removeTrack({ part->id(), pair.second->id().toStdString() });
        }
    });
}

void PlaybackController::setupSequencePlayer()
{
    PlaybackCursorType cursorType = configuration()->cursorType();

    playback()->player()->playbackPositionMsecs().onReceive(this, [this, cursor = std::move(cursorType)]
                                                            (const TrackSequenceId id, const audio::msecs_t& msecs) {
        if (m_currentSequenceId != id) {
            return;
        }

        tick_t tick = notationPlayback()->secToTick(msecs / 1000.);

        setCurrentTick(tick);
        m_tickPlayed.send(std::move(tick));
    });

    playback()->player()->setDuration(m_currentSequenceId, notationPlayback()->totalPlayTime());

    playback()->player()->playbackStatusChanged().onReceive(this, [this](const TrackSequenceId id, const PlaybackStatus status) {
        if (m_currentSequenceId != id) {
            return;
        }

        m_currentPlaybackStatus = status;

        if (m_currentPlaybackStatus == PlaybackStatus::Paused) {
            m_isPlaying = false;
            m_isPlayingChanged.notify();
        }
    });
}

bool PlaybackController::actionChecked(const ActionCode& actionCode) const
{
    QMap<std::string, bool> isChecked {
        { LOOP_CODE, isLoopVisible() },
        { MIDI_ON_CODE, notationConfiguration()->isMidiInputEnabled() },
        { REPEAT_CODE, notationConfiguration()->isPlayRepeatsEnabled() },
        { PAN_CODE, notationConfiguration()->isAutomaticallyPanEnabled() },
        { METRONOME_CODE, notationConfiguration()->isMetronomeEnabled() },
        { COUNT_IN_CODE, notationConfiguration()->isCountInEnabled() }
    };

    return isChecked[actionCode];
}

Channel<ActionCode> PlaybackController::actionCheckedChanged() const
{
    return m_actionCheckedChanged;
}

QTime PlaybackController::totalPlayTime() const
{
    QTime result = ZERO_TIME;

    if (!notationPlayback()) {
        return result;
    }

    return result.addMSecs(notationPlayback()->totalPlayTime());
}

Tempo PlaybackController::currentTempo() const
{
    return notationPlayback() ? notationPlayback()->tempo(currentTick()) : Tempo();
}

MeasureBeat PlaybackController::currentBeat() const
{
    return notationPlayback() ? notationPlayback()->beat(currentTick()) : MeasureBeat();
}

msecs_t PlaybackController::beatToMilliseconds(int measureIndex, int beatIndex) const
{
    if (!notationPlayback()) {
        return 0;
    }

    int tick = notationPlayback()->beatToTick(measureIndex, beatIndex);
    msecs_t milliseconds = secondsToMilliseconds(notationPlayback()->playedTickToSec(tick));

    return milliseconds;
}
