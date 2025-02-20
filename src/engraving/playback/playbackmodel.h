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

#ifndef MU_ENGRAVING_PLAYBACKMODEL_H
#define MU_ENGRAVING_PLAYBACKMODEL_H

#include <unordered_map>
#include <map>
#include <functional>

#include "async/asyncable.h"
#include "async/channel.h"
#include "id.h"
#include "modularity/ioc.h"
#include "mpe/events.h"
#include "mpe/iarticulationprofilesrepository.h"

#include "types/types.h"
#include "playbackeventsrenderer.h"
#include "playbacksetupdataresolver.h"
#include "playbackcontext.h"

namespace Ms {
class Score;
class Note;
class EngravingItem;
class Segment;
class Instrument;
class RepeatList;
}

namespace mu::engraving {
class PlaybackModel : public async::Asyncable
{
    INJECT(engraving, mpe::IArticulationProfilesRepository, profilesRepository)

public:
    void load(Ms::Score* score, async::Channel<int, int, int, int> notationChangesRangeChannel);
    void reload();

    bool isPlayRepeatsEnabled() const;
    void setPlayRepeats(const bool isEnabled);

    const InstrumentTrackId& metronomeTrackId() const;

    const mpe::PlaybackData& resolveTrackPlaybackData(const InstrumentTrackId& trackId);
    const mpe::PlaybackData& resolveTrackPlaybackData(const ID& partId, const std::string& instrumentId);
    void triggerEventsForItem(const Ms::EngravingItem* item);

private:
    static const InstrumentTrackId METRONOME_TRACK_ID;

    using ChangedTrackIdSet = InstrumentTrackIdSet;

    InstrumentTrackId idKey(const Ms::EngravingItem* item) const;
    InstrumentTrackId idKey(const ID& partId, const std::string& instrimentId) const;

    void update(const int tickFrom, const int tickTo, const int trackFrom, const int trackTo, ChangedTrackIdSet* trackChanges = nullptr);
    void updateSetupData();
    void updateEvents(const int tickFrom, const int tickTo, const int trackFrom, const int trackTo,
                      ChangedTrackIdSet* trackChanges = nullptr);
    void clearExpiredTracks();
    void clearExpiredContexts();
    void clearExpiredEvents(const Ms::Segment* segment, const int tickPosition, const int trackFrom, const int trackTo);
    void collectChangesTracks(const InstrumentTrackId& trackId, ChangedTrackIdSet* result);
    void notifyAboutChanges(ChangedTrackIdSet&& trackChanges);

    void removeEvents(const InstrumentTrackId& trackId, const mpe::timestamp_t timestamp);

    void findEventsForNote(const Ms::Note* note, const mpe::PlaybackEventList& sourceEvents, mpe::PlaybackEventList& result) const;

    const Ms::RepeatList& repeatList() const;

    Ms::Score* m_score = nullptr;
    bool m_expandRepeats = true;

    PlaybackEventsRenderer m_renderer;
    PlaybackSetupDataResolver m_setupResolver;

    std::unordered_map<InstrumentTrackId, PlaybackContext> m_playbackCtxMap;
    std::unordered_map<InstrumentTrackId, mpe::PlaybackData> m_playbackDataMap;
};
}

#endif // PLAYBACKMODEL_H
