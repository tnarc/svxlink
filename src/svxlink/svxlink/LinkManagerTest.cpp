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
      // and record the scheduled-announcement classification seen at play time.
    void playFile(const std::string&) override
    {
      ++files_played;
      saw_scheduled = scheduledAnnouncement();
    }

    int files_played = 0;
    bool saw_scheduled = false;

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

      lm->instance();
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
  test_announce_all_exclude();
  test_announce_scheduled_propagation();

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
