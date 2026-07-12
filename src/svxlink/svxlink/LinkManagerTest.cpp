/**
@file    LinkManagerTest.cpp
@brief   Unit tests for the LinkManager audio modes (MIX/DUCK/PRIORITY),
         PRIORITY_HANGTIME and announce-on-all-logics broadcast routing,
         driving the routing and gain logic directly without audio.
@author  Mark Rose
@date    2026-06-06

These tests construct a LinkManager with lightweight fake logic cores and
inspect the per-connection valve open/closed state
(LinkManager::linkValveOpen) and mixer-amp gain (LinkManager::linkGain) to
assert the MIX, DUCK and PRIORITY behaviour, including the PRIORITY_HANGTIME
release timing that the audio-level integration tests cannot observe. They
also verify that broadcast announcements skip ANNOUNCE_ALL_EXCLUDE logics.

\verbatim
SvxLink - A Multi Purpose Voice Services System for Ham Radio Use
Copyright (C) 2026 Tobias Blomberg / SM0SVX

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
\endverbatim
*/

#include <AsyncCppApplication.h>
#include <AsyncConfig.h>
#include <AsyncTimer.h>
#include <AsyncAudioPassthrough.h>

#include <cmath>
#include <iostream>
#include <string>

#include "LinkManager.h"
#include "LogicBase.h"

using namespace std;
using namespace Async;


/****************************************************************************
 * Test helpers
 ****************************************************************************/

namespace {

int failures = 0;

void check(bool cond, const string& msg)
{
  cout << (cond ? "  ok   " : "  FAIL ") << msg << endl;
  if (!cond)
  {
    ++failures;
  }
}

bool near_db(float a, float b, float tol = 0.5f)
{
  return std::fabs(a - b) < tol;
}


/**
 * @brief A minimal LogicBase used as a stand-in for a real logic core.
 *
 * It only provides the audio connection endpoints the LinkManager needs to
 * build its switching matrix; no audio actually flows during these tests.
 */
class FakeLogic : public LogicBase
{
  public:
    FakeLogic(void)
      : m_in(new AudioPassthrough), m_out(new AudioPassthrough) {}
    ~FakeLogic(void) override { delete m_in; delete m_out; }
    AudioSink *logicConIn(void) override { return m_in; }
    AudioSource *logicConOut(void) override { return m_out; }

      // Count broadcast playback so tests can assert which logics were reached,
      // and record the scheduled-announcement classification and forced-CTCSS
      // state seen at play time. When announce_on_all_logics is set, mirror
      // the real Logic::playFile broadcast tail (guarded by deferralSuppressed
      // exactly as the real one is) so the re-entrancy guard can be exercised
      // without a full Logic.
    void playFile(const std::string& path) override
    {
      ++files_played;
      saw_scheduled = scheduledAnnouncement();
      saw_force = forceCtcss();
      if (announce_on_all_logics && !deferralSuppressed() &&
          LinkManager::hasInstance())
      {
        LinkManager::instance()->playFileAll(this, path);
      }
    }

    int files_played = 0;
    bool saw_scheduled = false;
    bool saw_force = false;
    bool announce_on_all_logics = false;

  private:
    AudioPassthrough *m_in;
    AudioPassthrough *m_out;
};


/**
 * @brief Build a LinkManager with three logics and the given link sections.
 *
 * Logics are named Logic1..Logic3. The caller owns the returned logics and
 * must delete them AFTER LinkManager::deleteInstance().
 */
void buildLinks(Config& cfg, const string& links, FakeLogic* logics[3])
{
  LinkManager::initialize(cfg, links);
  const char* names[3] = {"Logic1", "Logic2", "Logic3"};
  for (int i = 0; i < 3; ++i)
  {
    cfg.setValue(names[i], "TYPE", string("Test"));
    logics[i] = new FakeLogic;
    logics[i]->initialize(cfg, names[i]);
  }
  LinkManager::instance()->allLogicsStarted();
}

void teardown(FakeLogic* logics[3])
{
  LinkManager::deleteInstance();
  for (int i = 0; i < 3; ++i)
  {
    delete logics[i];
    logics[i] = nullptr;
  }
}


/****************************************************************************
 * Tests
 ****************************************************************************/

// MIX: both source valves open into the listener's mixer.
void test_mix_opens_valves(void)
{
  cout << "test_mix_opens_valves" << endl;
  Config cfg;
  cfg.setValue("L", "CONNECT_LOGICS", string("Logic1,Logic2,Logic3"));
  cfg.setValue("L", "AUDIO_MODE", string("MIX"));
  cfg.setValue("L", "DEFAULT_ACTIVE", string("1"));
  FakeLogic* lg[3];
  buildLinks(cfg, "L", lg);
  LinkManager* lm = LinkManager::instance();

  check(lm->linkValveOpen("Logic1", "Logic3"), "Logic1->Logic3 valve open");
  check(lm->linkValveOpen("Logic2", "Logic3"), "Logic2->Logic3 valve open");
  teardown(lg);
}

// DUCK: incoming amp drops to DUCK_LEVEL_DB while the sink squelch is open,
// and is restored to 0 dB when it closes.
void test_duck_gain(void)
{
  cout << "test_duck_gain" << endl;
  Config cfg;
  cfg.setValue("L", "CONNECT_LOGICS", string("Logic1,Logic2,Logic3"));
  cfg.setValue("L", "AUDIO_MODE", string("DUCK"));
  cfg.setValue("L", "DUCK_LEVEL_DB", string("-15"));
  cfg.setValue("L", "DEFAULT_ACTIVE", string("1"));
  FakeLogic* lg[3];
  buildLinks(cfg, "L", lg);
  LinkManager* lm = LinkManager::instance();

  check(near_db(lm->linkGain("Logic2", "Logic1"), 0.0f),
        "no duck before squelch opens");
  lg[0]->squelchStateChanged(true);                  // Logic1 squelch opens
  check(near_db(lm->linkGain("Logic2", "Logic1"), -15.0f),
        "Logic2->Logic1 ducked to -15 dB");
  lg[0]->squelchStateChanged(false);                 // Logic1 squelch closes
  check(near_db(lm->linkGain("Logic2", "Logic1"), 0.0f),
        "duck released to 0 dB");
  teardown(lg);
}

// PRIORITY: a non-priority source is muted to PRIORITY_MUTE_DB while a
// priority-link source transmits; the priority source stays at 0 dB.
void test_priority_gain(void)
{
  cout << "test_priority_gain" << endl;
  Config cfg;
  cfg.setValue("Pri", "CONNECT_LOGICS", string("Logic1,Logic3"));
  cfg.setValue("Pri", "AUDIO_MODE", string("PRIORITY"));
  cfg.setValue("Pri", "PRIORITY_MUTE_DB", string("-30"));
  cfg.setValue("Pri", "DEFAULT_ACTIVE", string("1"));
  cfg.setValue("Norm", "CONNECT_LOGICS", string("Logic2,Logic3"));
  cfg.setValue("Norm", "AUDIO_MODE", string("MIX"));
  cfg.setValue("Norm", "DEFAULT_ACTIVE", string("1"));
  FakeLogic* lg[3];
  buildLinks(cfg, "Pri,Norm", lg);
  LinkManager* lm = LinkManager::instance();

  check(near_db(lm->linkGain("Logic2", "Logic3"), 0.0f),
        "normal source full before priority");
  lg[0]->squelchStateChanged(true);                  // priority source active
  check(near_db(lm->linkGain("Logic2", "Logic3"), -30.0f),
        "non-priority muted to -30 dB");
  check(near_db(lm->linkGain("Logic1", "Logic3"), 0.0f),
        "priority source stays at 0 dB");
  lg[0]->squelchStateChanged(false);                 // priority stops (no hangtime)
  check(near_db(lm->linkGain("Logic2", "Logic3"), 0.0f),
        "non-priority restored immediately (no hangtime)");
  teardown(lg);
}

// A member's OWN local transmission must not arm hangtime muting of that
// member's incoming audio. Logic3 is the "hub": it is in a PRIORITY link with
// Logic1 and also receives a MIX source (Logic2). When a local user talks on
// Logic3, Logic3 is a priority source into Logic1 (not into itself), so the
// hangtime that follows applies to Logic1, never to Logic3 - Logic2->Logic3
// must stay unmuted throughout, even with PRIORITY_HANGTIME configured.
void test_hub_traffic_no_hangtime_mute(void)
{
  cout << "test_hub_traffic_no_hangtime_mute" << endl;
  Config cfg;
  cfg.setValue("Pri", "CONNECT_LOGICS", string("Logic1,Logic3"));
  cfg.setValue("Pri", "AUDIO_MODE", string("PRIORITY"));
  cfg.setValue("Pri", "PRIORITY_MUTE_DB", string("-30"));
  cfg.setValue("Pri", "PRIORITY_HANGTIME", string("300"));
  cfg.setValue("Pri", "DEFAULT_ACTIVE", string("1"));
  cfg.setValue("Norm", "CONNECT_LOGICS", string("Logic2,Logic3"));
  cfg.setValue("Norm", "AUDIO_MODE", string("MIX"));
  cfg.setValue("Norm", "DEFAULT_ACTIVE", string("1"));
  FakeLogic* lg[3];
  buildLinks(cfg, "Pri,Norm", lg);
  LinkManager* lm = LinkManager::instance();

  check(near_db(lm->linkGain("Logic2", "Logic3"), 0.0f),
        "MIX source full before any local traffic");

    // Local traffic on the hub (Logic3): Logic3 is a source, not a sink for
    // its own audio, so its incoming MIX source must stay unmuted.
  lg[2]->squelchStateChanged(true);
  check(near_db(lm->linkGain("Logic2", "Logic3"), 0.0f),
        "hub local traffic does not mute the hub's own incoming audio");

    // Unkey the hub: the buggy behaviour armed hangtime keyed off the hub's own
    // squelch and muted Logic2->Logic3 for the whole hangtime. It must not.
  lg[2]->squelchStateChanged(false);
  check(near_db(lm->linkGain("Logic2", "Logic3"), 0.0f),
        "hub local transmission does not arm hangtime muting on unkey");
  teardown(lg);
}

// Deactivating a PRIORITY link while its hangtime timer is running must reset
// the hangtime state: the held-muted sinks are restored and the timer stopped,
// so reactivating the link does not resurrect a spurious mute from a frozen
// timer.
void test_priority_hangtime_reset_on_deactivate(void)
{
  cout << "test_priority_hangtime_reset_on_deactivate" << endl;
  Config cfg;
  cfg.setValue("Pri", "CONNECT_LOGICS", string("Logic1,Logic3"));
  cfg.setValue("Pri", "AUDIO_MODE", string("PRIORITY"));
  cfg.setValue("Pri", "PRIORITY_MUTE_DB", string("-30"));
  cfg.setValue("Pri", "PRIORITY_HANGTIME", string("300"));
  cfg.setValue("Pri", "DEFAULT_ACTIVE", string("1"));
  cfg.setValue("Norm", "CONNECT_LOGICS", string("Logic2,Logic3"));
  cfg.setValue("Norm", "AUDIO_MODE", string("MIX"));
  cfg.setValue("Norm", "DEFAULT_ACTIVE", string("1"));
  FakeLogic* lg[3];
  buildLinks(cfg, "Pri,Norm", lg);
  LinkManager* lm = LinkManager::instance();

    // Priority source transmits then unkeys: hangtime arms, Logic2->Logic3
    // held muted.
  lg[0]->squelchStateChanged(true);
  check(near_db(lm->linkGain("Logic2", "Logic3"), -30.0f),
        "muted while priority active");
  lg[0]->squelchStateChanged(false);
  check(near_db(lm->linkGain("Logic2", "Logic3"), -30.0f),
        "still muted at hangtime start");

    // Deactivate the PRIORITY link mid-hangtime: the mute must be released and
    // the timer stopped.
  lm->deactivateLinkByName("Pri");
  check(near_db(lm->linkGain("Logic2", "Logic3"), 0.0f),
        "mute released when link deactivated mid-hangtime");

    // Reactivate: the frozen timer must not resurrect the mute.
  lm->activateLinkByName("Pri");
  check(near_db(lm->linkGain("Logic2", "Logic3"), 0.0f),
        "no spurious mute after reactivation");
  teardown(lg);
}

// Announce-on-all-logics: a broadcast (playFileAll) plays on every logic
// except the source and any logic with ANNOUNCE_ALL_EXCLUDE set.
void test_announce_all_exclude(void)
{
  cout << "test_announce_all_exclude" << endl;
  Config cfg;
  cfg.setValue("L", "CONNECT_LOGICS", string("Logic1,Logic2,Logic3"));
    // Logic2 opts out of receiving broadcast announcements.
  cfg.setValue("Logic2", "ANNOUNCE_ALL_EXCLUDE", string("1"));
  FakeLogic* lg[3];
  buildLinks(cfg, "L", lg);
  LinkManager* lm = LinkManager::instance();

    // Broadcast from Logic1.
  lm->playFileAll(lg[0], "dummy.wav");
  check(lg[0]->files_played == 0, "source logic is not played to");
  check(lg[1]->files_played == 0, "ANNOUNCE_ALL_EXCLUDE logic is skipped");
  check(lg[2]->files_played == 1, "other logic receives the broadcast");
  teardown(lg);
}

// A scheduled announcement broadcast propagates the scheduled classification
// to each target (so they suppress CTCSS the same way), and restores the
// target's state afterwards.
void test_announce_scheduled_propagation(void)
{
  cout << "test_announce_scheduled_propagation" << endl;
  Config cfg;
  cfg.setValue("L", "CONNECT_LOGICS", string("Logic1,Logic2,Logic3"));
  cfg.setValue("Logic2", "ANNOUNCE_ALL_EXCLUDE", string("1"));
  FakeLogic* lg[3];
  buildLinks(cfg, "L", lg);
  LinkManager* lm = LinkManager::instance();

    // Scheduled broadcast from Logic1: Logic3 should play as scheduled.
  lg[0]->setScheduledAnnouncement(true);
  lm->playFileAll(lg[0], "dummy.wav");
  check(lg[2]->files_played == 1 && lg[2]->saw_scheduled,
        "target plays scheduled when source is scheduled");
  check(!lg[2]->scheduledAnnouncement(),
        "target scheduled flag restored after play");
  check(lg[1]->files_played == 0, "excluded logic still skipped");

    // Non-scheduled broadcast: Logic3 should play as not scheduled.
  lg[0]->setScheduledAnnouncement(false);
  lm->playFileAll(lg[0], "dummy.wav");
  check(lg[2]->files_played == 2 && !lg[2]->saw_scheduled,
        "target plays non-scheduled when source is not scheduled");
  teardown(lg);
}

// A deferred announcement that is replayed forces the CTCSS tone on. When such
// an announcement is broadcast to all logics, the forced-CTCSS state must be
// propagated to each target so listeners filtering on CTCSS hear it on every
// port. Unlike the scheduled flag, forceCtcss is deliberately NOT restored
// after the play call: the target plays the mirrored audio asynchronously, so
// the flag must stay set for the whole playback and is cleared by the target's
// own allMsgsWritten when the announcement drains (a real Logic; the FakeLogic
// here does not model that, so the flag remains set after the call).
void test_announce_force_ctcss_propagation(void)
{
  cout << "test_announce_force_ctcss_propagation" << endl;
  Config cfg;
  cfg.setValue("L", "CONNECT_LOGICS", string("Logic1,Logic2,Logic3"));
  cfg.setValue("Logic2", "ANNOUNCE_ALL_EXCLUDE", string("1"));
  FakeLogic* lg[3];
  buildLinks(cfg, "L", lg);
  LinkManager* lm = LinkManager::instance();

    // Forced-CTCSS broadcast from Logic1: Logic3 should see force at play time
    // and the flag must remain set afterwards (left for its own allMsgsWritten).
  lg[0]->setForceCtcss(true);
  lm->playFileAll(lg[0], "dummy.wav");
  check(lg[2]->files_played == 1 && lg[2]->saw_force,
        "target sees forced CTCSS when source forces it");
  check(lg[2]->forceCtcss(),
        "target force-CTCSS flag left set for async playback (not restored)");
  check(lg[1]->files_played == 0, "excluded logic still skipped");

    // A non-forced broadcast clears it again (force is set to the source value
    // on every broadcast), and the target does not see force at play time.
  lg[0]->setForceCtcss(false);
  lm->playFileAll(lg[0], "dummy.wav");
  check(lg[2]->files_played == 2 && !lg[2]->saw_force,
        "target does not see forced CTCSS when source does not force it");
  check(!lg[2]->forceCtcss(),
        "non-forced broadcast leaves the target un-forced");
  teardown(lg);
}

// A mirrored broadcast play must not itself re-broadcast: if two (or more)
// logics both have announce-on-all-logics enabled, a single top-level play
// must reach every other logic exactly once, not recurse back and forth
// between them. LinkManager::playFileAll suppresses deferral
// (setDeferralSuppressed(true)) around each mirrored play specifically so the
// mirrored logic's own broadcast tail can detect "this is a mirror, don't
// re-broadcast" and skip itself.
void test_announce_broadcast_no_recursion(void)
{
  cout << "test_announce_broadcast_no_recursion" << endl;
  Config cfg;
  cfg.setValue("L", "CONNECT_LOGICS", string("Logic1,Logic2,Logic3"));
  FakeLogic* lg[3];
  buildLinks(cfg, "L", lg);

    // Logic1 and Logic2 both broadcast to every other logic; Logic3 does not.
  lg[0]->announce_on_all_logics = true;
  lg[1]->announce_on_all_logics = true;

    // Top-level play on Logic1 (as if called from Logic::playFile itself).
  lg[0]->playFile("dummy.wav");

  check(lg[0]->files_played == 1, "source plays exactly once, no recursion");
  check(lg[1]->files_played == 1,
        "mirrored target plays exactly once, its own broadcast tail is "
        "suppressed for the mirror");
  check(lg[2]->files_played == 1, "non-broadcasting target plays exactly once");
  teardown(lg);
}

// Overlapping links with different modes on a shared sink. Link B (PRIORITY,
// Logic3->Logic1) overrides link A (MIX) on the 3->1 path, so for sink Logic1
// the source Logic3 is PRIORITY and the source Logic2 is MIX. When the priority
// source transmits, the non-priority MIX source into Logic1 must be muted to
// PRIORITY_MUTE_DB and restored again afterwards.
void test_overlapping_links_different_modes(void)
{
  cout << "test_overlapping_links_different_modes" << endl;
  Config cfg;
  // Link A: MIX between Logic1, Logic2, Logic3
  cfg.setValue("A", "CONNECT_LOGICS", string("Logic1,Logic2,Logic3"));
  cfg.setValue("A", "AUDIO_MODE", string("MIX"));
  cfg.setValue("A", "DEFAULT_ACTIVE", string("1"));
  // Link B: PRIORITY from Logic3 to Logic1 (overlaps A on the 3->1 path)
  cfg.setValue("B", "CONNECT_LOGICS", string("Logic3,Logic1"));
  cfg.setValue("B", "AUDIO_MODE", string("PRIORITY"));
  cfg.setValue("B", "PRIORITY_MUTE_DB", string("-25"));
  cfg.setValue("B", "DEFAULT_ACTIVE", string("1"));
  FakeLogic* lg[3];
  buildLinks(cfg, "A,B", lg);
  LinkManager* lm = LinkManager::instance();

  check(near_db(lm->linkGain("Logic2", "Logic1"), 0.0f),
        "MIX source full before priority");
  check(near_db(lm->linkGain("Logic3", "Logic1"), 0.0f),
        "priority source full before it transmits");

  // Priority source Logic3 transmits into Logic1.
  lg[2]->squelchStateChanged(true);
  check(near_db(lm->linkGain("Logic3", "Logic1"), 0.0f),
        "priority source stays full while transmitting");
  check(near_db(lm->linkGain("Logic2", "Logic1"), -25.0f),
        "non-priority MIX source muted to -25 dB while priority active");

  // Priority source stops (no hangtime configured): the MIX source is restored.
  lg[2]->squelchStateChanged(false);
  check(near_db(lm->linkGain("Logic2", "Logic1"), 0.0f),
        "non-priority MIX source restored after priority stops");
  teardown(lg);
}

// A squelch event must not resurrect audio on a deactivated or unlinked
// connection. onSquelchStateChanged calls updatePriorityForSink for every sink;
// that must only touch connections that belong to an activated link, otherwise
// the first squelch event anywhere cross-connects logics behind the link
// manager's back.
void test_squelch_does_not_crossconnect(void)
{
  cout << "test_squelch_does_not_crossconnect" << endl;
  Config cfg;
  // Link A active: FIRST between Logic1 and Logic2.
  cfg.setValue("A", "CONNECT_LOGICS", string("Logic1,Logic2"));
  cfg.setValue("A", "AUDIO_MODE", string("FIRST"));
  cfg.setValue("A", "DEFAULT_ACTIVE", string("1"));
  // Link B deactivated: FIRST between Logic2 and Logic3.
  cfg.setValue("B", "CONNECT_LOGICS", string("Logic2,Logic3"));
  cfg.setValue("B", "AUDIO_MODE", string("FIRST"));
  cfg.setValue("B", "DEFAULT_ACTIVE", string("0"));
  FakeLogic* lg[3];
  buildLinks(cfg, "A,B", lg);
  LinkManager* lm = LinkManager::instance();

  // Baseline: the active link is connected, the deactivated/unlinked pairs are
  // not.
  check(lm->linkSelectorEnabled("Logic1", "Logic2"),
        "active FIRST link selector enabled");
  check(!lm->linkSelectorEnabled("Logic2", "Logic3"),
        "deactivated link selector disabled before squelch");
  check(!lm->linkSelectorEnabled("Logic1", "Logic3"),
        "unlinked pair selector disabled before squelch");

  // A squelch event anywhere must NOT enable the deactivated/unlinked
  // connections.
  lg[0]->squelchStateChanged(true);
  check(lm->linkSelectorEnabled("Logic1", "Logic2"),
        "active link still connected after squelch");
  check(!lm->linkSelectorEnabled("Logic2", "Logic3"),
        "deactivated link NOT cross-connected by squelch event");
  check(!lm->linkSelectorEnabled("Logic3", "Logic2"),
        "deactivated link NOT cross-connected by squelch event (reverse)");
  check(!lm->linkSelectorEnabled("Logic1", "Logic3"),
        "unlinked pair NOT cross-connected by squelch event");
  check(!lm->linkValveOpen("Logic2", "Logic3"),
        "deactivated link valve stays closed after squelch event");

  lg[0]->squelchStateChanged(false);
  teardown(lg);
}

// A sink that is simultaneously in an activated FIRST link and an activated MIX
// link must keep BOTH sources audible: the MIX source through the mixer and the
// FIRST source through the selector, which is routed INTO the mixer rather than
// disconnected (which would silently discard the FIRST audio).
void test_overlapping_first_and_mix(void)
{
  cout << "test_overlapping_first_and_mix" << endl;
  Config cfg;
  // Link A: FIRST between Logic1 and Logic2.
  cfg.setValue("A", "CONNECT_LOGICS", string("Logic1,Logic2"));
  cfg.setValue("A", "AUDIO_MODE", string("FIRST"));
  cfg.setValue("A", "DEFAULT_ACTIVE", string("1"));
  // Link B: MIX between Logic1 and Logic3 (overlaps on sink Logic1).
  cfg.setValue("B", "CONNECT_LOGICS", string("Logic1,Logic3"));
  cfg.setValue("B", "AUDIO_MODE", string("MIX"));
  cfg.setValue("B", "DEFAULT_ACTIVE", string("1"));
  FakeLogic* lg[3];
  buildLinks(cfg, "A,B", lg);
  LinkManager* lm = LinkManager::instance();

  // For sink Logic1: Logic2 is FIRST (selector path), Logic3 is MIX (mixer).
  check(lm->linkSelectorEnabled("Logic2", "Logic1"),
        "FIRST source uses the selector into Logic1");
  check(!lm->linkValveOpen("Logic2", "Logic1"),
        "FIRST source does not open a mixer valve");
  check(lm->linkValveOpen("Logic3", "Logic1"),
        "MIX source opens its mixer valve into Logic1");

  // The mixer drives Logic1's input, and the selector (carrying the FIRST
  // source) is routed into the mixer so the FIRST audio is preserved, not lost.
  check(lm->sinkSelectorRoutedToMixer("Logic1"),
        "selector routed into the mixer so FIRST audio is not discarded");
  teardown(lg);
}

// The effective audio mode of an already-established connection must be
// re-evaluated when an overlapping link changes it, in either activation
// direction. Activating a MIX link over an existing FIRST connection moves that
// connection onto the mixer path; deactivating it moves the connection back.
void test_overlapping_activation_reconciles_mode(void)
{
  cout << "test_overlapping_activation_reconciles_mode" << endl;
  Config cfg;
  // Link A: FIRST between Logic1 and Logic2, active by default.
  cfg.setValue("A", "CONNECT_LOGICS", string("Logic1,Logic2"));
  cfg.setValue("A", "AUDIO_MODE", string("FIRST"));
  cfg.setValue("A", "DEFAULT_ACTIVE", string("1"));
  // Link B: MIX between the same pair, initially deactivated.
  cfg.setValue("B", "CONNECT_LOGICS", string("Logic1,Logic2"));
  cfg.setValue("B", "AUDIO_MODE", string("MIX"));
  cfg.setValue("B", "DEFAULT_ACTIVE", string("0"));
  FakeLogic* lg[3];
  buildLinks(cfg, "A,B", lg);
  LinkManager* lm = LinkManager::instance();

  // Only A active: the 2->1 connection is FIRST (selector, valve closed).
  check(lm->linkSelectorEnabled("Logic2", "Logic1"),
        "FIRST-only: selector enabled");
  check(!lm->linkValveOpen("Logic2", "Logic1"),
        "FIRST-only: mixer valve closed");

  // Activate the overlapping MIX link. The already-established 2->1 connection
  // must be re-evaluated to MIX (mixer valve open, selector branch disabled)
  // even though it was never removed from the current connection set.
  lm->activateLinkByName("B");
  check(lm->linkValveOpen("Logic2", "Logic1"),
        "after MIX link activation: connection moved to mixer path");
  check(!lm->linkSelectorEnabled("Logic2", "Logic1"),
        "after MIX link activation: selector branch disabled");

  // Deactivate the MIX link again: the connection must revert to FIRST.
  lm->deactivateLinkByName("B");
  check(!lm->linkValveOpen("Logic2", "Logic1"),
        "after MIX link deactivation: connection reverts to selector path");
  check(lm->linkSelectorEnabled("Logic2", "Logic1"),
        "after MIX link deactivation: selector branch re-enabled");
  teardown(lg);
}

// Bad AUDIO_MODE should warn but fall back to FIRST (no mixer valves).
void test_bad_audio_mode_falls_back(void)
{
  cout << "test_bad_audio_mode_falls_back" << endl;
  Config cfg;
  cfg.setValue("L", "CONNECT_LOGICS", string("Logic1,Logic2,Logic3"));
  cfg.setValue("L", "AUDIO_MODE", string("FOOBAR"));  // invalid
  cfg.setValue("L", "DEFAULT_ACTIVE", string("1"));
  FakeLogic* lg[3];
  buildLinks(cfg, "L", lg);
  LinkManager* lm = LinkManager::instance();

  // Should behave as FIRST: no mixer valves open for second source
  lg[0]->squelchStateChanged(true);
  check(!lm->linkValveOpen("Logic2", "Logic1"),
        "invalid AUDIO_MODE falls back to FIRST (valve closed)");
  teardown(lg);
}

// No AUDIO_MODE at all must be exactly FIRST (backward compat).
void test_no_audio_mode_is_first(void)
{
  cout << "test_no_audio_mode_is_first" << endl;
  Config cfg;
  cfg.setValue("L", "CONNECT_LOGICS", string("Logic1,Logic2,Logic3"));
  // deliberately no AUDIO_MODE
  cfg.setValue("L", "DEFAULT_ACTIVE", string("1"));
  FakeLogic* lg[3];
  buildLinks(cfg, "L", lg);
  LinkManager* lm = LinkManager::instance();

  // FIRST behavior: only first source selected via selector, no mixer valves
  lg[0]->squelchStateChanged(true);
  check(!lm->linkValveOpen("Logic2", "Logic1"),
        "no AUDIO_MODE keeps FIRST mode (second source valve closed)");
  // A second source should not affect the first's path in FIRST
  lg[1]->squelchStateChanged(true);
  check(!lm->linkValveOpen("Logic2", "Logic1"),
        "no AUDIO_MODE: second source ignored in FIRST");
  teardown(lg);
}

/**
 * @brief PRIORITY_HANGTIME test - needs the event loop for the timer.
 *
 * After the priority source stops, the non-priority source stays muted for
 * PRIORITY_HANGTIME ms, then is restored.
 */
class HangtimeTest : public sigc::trackable
{
  public:
    void run(void)
    {
      cout << "test_priority_hangtime" << endl;
      m_cfg.setValue("Pri", "CONNECT_LOGICS", string("Logic1,Logic3"));
      m_cfg.setValue("Pri", "AUDIO_MODE", string("PRIORITY"));
      m_cfg.setValue("Pri", "PRIORITY_MUTE_DB", string("-30"));
      m_cfg.setValue("Pri", "PRIORITY_HANGTIME", string("300"));
      m_cfg.setValue("Pri", "DEFAULT_ACTIVE", string("1"));
      m_cfg.setValue("Norm", "CONNECT_LOGICS", string("Logic2,Logic3"));
      m_cfg.setValue("Norm", "AUDIO_MODE", string("MIX"));
      m_cfg.setValue("Norm", "DEFAULT_ACTIVE", string("1"));
      buildLinks(m_cfg, "Pri,Norm", m_lg);
      LinkManager* lm = LinkManager::instance();

      m_lg[0]->squelchStateChanged(true);            // priority active
      check(near_db(lm->linkGain("Logic2", "Logic3"), -30.0f),
            "muted while priority active");
      m_lg[0]->squelchStateChanged(false);           // priority stops -> hangtime
      check(near_db(lm->linkGain("Logic2", "Logic3"), -30.0f),
            "still muted at hangtime start");

        // Mid-hangtime check (150 ms < 300 ms): still muted
      m_mid = new Timer(150);
      m_mid->expired.connect(sigc::hide(
          sigc::mem_fun(*this, &HangtimeTest::midHangtime)));
        // Post-hangtime check (450 ms > 300 ms): restored
      m_post = new Timer(450);
      m_post->expired.connect(sigc::hide(
          sigc::mem_fun(*this, &HangtimeTest::postHangtime)));
    }

  private:
    Config m_cfg;
    FakeLogic* m_lg[3];
    Timer* m_mid = nullptr;
    Timer* m_post = nullptr;

    void midHangtime(void)
    {
      check(near_db(LinkManager::instance()->linkGain("Logic2", "Logic3"),
                    -30.0f),
            "still muted mid-hangtime (150 ms)");
    }

    void postHangtime(void)
    {
      check(near_db(LinkManager::instance()->linkGain("Logic2", "Logic3"),
                    0.0f),
            "restored after hangtime (450 ms)");
      delete m_mid;
      delete m_post;
      teardown(m_lg);
      Application::app().quit();
    }
};

} /* anonymous namespace */


int main(void)
{
  CppApplication app;

    // Synchronous tests (no timers needed)
  test_mix_opens_valves();
  test_duck_gain();
  test_priority_gain();
  test_hub_traffic_no_hangtime_mute();
  test_priority_hangtime_reset_on_deactivate();
  test_announce_all_exclude();
  test_announce_scheduled_propagation();
  test_announce_force_ctcss_propagation();
  test_announce_broadcast_no_recursion();
  test_overlapping_links_different_modes();
  test_squelch_does_not_crossconnect();
  test_overlapping_first_and_mix();
  test_overlapping_activation_reconciles_mode();
  test_bad_audio_mode_falls_back();
  test_no_audio_mode_is_first();

    // Event-loop test for hangtime; quits the app when done
  HangtimeTest hangtime;
  hangtime.run();
  app.exec();

  cout << endl;
  if (failures == 0)
  {
    cout << "All LinkManager tests passed" << endl;
    return 0;
  }
  cout << failures << " LinkManager test check(s) FAILED" << endl;
  return 1;
}
