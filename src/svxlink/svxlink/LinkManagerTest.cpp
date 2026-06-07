/**
@file    LinkManagerTest.cpp
@brief   Unit tests for the LinkManager audio modes (MIX) driving the routing
         logic directly without audio.
@author  Mark Rose
@date    2026-06-06

These tests construct a LinkManager with lightweight fake logic cores and
inspect the per-connection valve open/closed state
(LinkManager::linkValveOpen) to assert the MIX behaviour.

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

} /* anonymous namespace */


int main(void)
{
  CppApplication app;

  test_mix_opens_valves();

  cout << endl;
  if (failures == 0)
  {
    cout << "All LinkManager tests passed" << endl;
    return 0;
  }
  cout << failures << " LinkManager test check(s) FAILED" << endl;
  return 1;
}
