/**
@file	 LinkManager.cpp
@brief   Contains the manager to link different logics together
         implemented as singleton
@author  Adi Bier (DL1HRC) / Christian Stussak (University of Halle/Saale)
         Tobias Blomberg / SM0SVX
@date	 2011-11-24

\verbatim
SvxLink - A Multi Purpose Voice Services System for Ham Radio Use
Copyright (C) 2003-2021 Tobias Blomberg / SM0SVX

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
\endverbatim
*/



/****************************************************************************
 *
 * System Includes
 *
 ****************************************************************************/

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <algorithm>
#include <iterator>
#include <regex>
#include <limits>


/****************************************************************************
 *
 * Project Includes
 *
 ****************************************************************************/

#include <AsyncConfig.h>
#include <AsyncTimer.h>
#include <common.h>
#include <AsyncAudioSource.h>
#include <AsyncAudioSink.h>
#include <AsyncAudioSplitter.h>
#include <AsyncAudioSelector.h>
#include <AsyncAudioPassthrough.h>
#include <AsyncAudioValve.h>
#include <AsyncAudioMixer.h>
#include <AsyncAudioAmp.h>


/****************************************************************************
 *
 * Local Includes
 *
 ****************************************************************************/

#include "LinkManager.h"
#include "CmdParser.h"
#include "Logic.h"
#include "LogicCmds.h"


/****************************************************************************
 *
 * Namespaces to use
 *
 ****************************************************************************/

using namespace std;
using namespace Async;


  // Put all locally defined types, functions and variables in an anonymous
  // namespace to make them file local. The "static" keyword has been
  // deprecated in C++ so it should not be used.
namespace
{

/****************************************************************************
 *
 * Defines & typedefs
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Prototypes for local functions
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Local Global Variables
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Local class definitions
 *
 ****************************************************************************/

  // The strength of an audio mode when several links create the same
  // connection. The strongest mode wins.
int modeStrength(LinkAudioMode mode)
{
  switch (mode)
  {
    case LinkAudioMode::PRIORITY: return 3;
    case LinkAudioMode::DUCK:     return 2;
    case LinkAudioMode::MIX:      return 1;
    case LinkAudioMode::FIRST:    break;
  }
  return 0;
} /* modeStrength */


/**
 * @brief Find the blocks of an undirected graph
 *
 * A block is a maximal biconnected subgraph (a single edge that is a bridge
 * is a block too). Every edge belongs to exactly one block. For two vertices
 * in a block and any edge of that block, there is a simple path between the
 * two vertices that passes the edge.
 */
class BlockFinder
{
  public:
    typedef std::map<std::string, std::set<std::string>> Graph;
    typedef std::pair<std::string, std::string> Edge;

    explicit BlockFinder(const Graph& graph) : m_graph(graph)
    {
      for (const auto& node : m_graph)
      {
        if (m_disc.count(node.first) == 0)
        {
          dfs(node.first, std::string());
        }
      }
    }

    const std::vector<std::vector<Edge>>& blocks(void) const
    {
      return m_blocks;
    }

  private:
    const Graph&                    m_graph;
    std::map<std::string, int>      m_disc;
    std::map<std::string, int>      m_low;
    int                             m_time = 0;
    std::vector<Edge>               m_stack;
    std::vector<std::vector<Edge>>  m_blocks;

      // Tarjan's algorithm. The recursion depth is bounded by the number of
      // logics, which is small.
    void dfs(const std::string& u, const std::string& parent)
    {
      m_disc[u] = m_low[u] = ++m_time;
      for (const auto& v : m_graph.at(u))
      {
        if (m_disc.count(v) == 0)
        {
          m_stack.push_back(Edge(u, v));
          dfs(v, u);
          m_low[u] = std::min(m_low[u], m_low[v]);
          if (m_low[v] >= m_disc[u])
          {
            std::vector<Edge> block;
            Edge e;
            do
            {
              e = m_stack.back();
              m_stack.pop_back();
              block.push_back(e);
            } while (e != Edge(u, v));
            m_blocks.push_back(block);
          }
        }
        else if ((v != parent) && (m_disc[v] < m_disc[u]))
        {
          m_stack.push_back(Edge(u, v));
          m_low[u] = std::min(m_low[u], m_disc[v]);
        }
      }
    }
};


} // End of anonymous namespace


/**
 * @brief Permanent feed into a logic's audio input
 *
 * The selector (FIRST mode) or the mixer (MIX/DUCK/PRIORITY) driving a logic
 * input is swapped by updateMixerRouting when connections change. An
 * AudioSource that is unregistered from its sink does not flush it, so when
 * the swap happened in the middle of an over the logic input never saw the
 * end of the stream and the transmitter stayed keyed. This feed sits between
 * the selector/mixer and the logic input, remembers whether a stream is open
 * and lets updateMixerRouting end it when the source is swapped out. The
 * completion of such an internally generated flush is not reported upstream,
 * since the source that is registered by then did not ask for it.
 */
class LinkManager::LogicInFeed : public AudioPassthrough
{
  public:
    int writeSamples(const float *samples, int count) override
    {
      m_stream_open = true;
      m_upstream_flush = false;
      return AudioPassthrough::writeSamples(samples, count);
    }

    void flushSamples(void) override
    {
      m_stream_open = false;
      m_upstream_flush = true;
      AudioPassthrough::flushSamples();
    }

    void allSamplesFlushed(void) override
    {
      if (m_upstream_flush)
      {
        m_upstream_flush = false;
        AudioPassthrough::allSamplesFlushed();
      }
    }

      // Call when the source feeding this object has just been unregistered
      // and before a new one is registered. If the old source left a stream
      // open, flush it so that the logic input sees the end of the stream.
    void endStream(void)
    {
      m_upstream_flush = false;
      if (m_stream_open)
      {
        m_stream_open = false;
        AudioPassthrough::flushSamples();
      }
    }

  private:
    bool m_stream_open = false;
    bool m_upstream_flush = false;
};

/****************************************************************************
 *
 * Exported Global Variables
 *
 ****************************************************************************/

LinkManager* LinkManager::_instance = 0;


/****************************************************************************
 *
 * Public member functions
 *
 ****************************************************************************/

bool LinkManager::initialize(Async::Config &cfg,
                             const std::string &link_names)
{
  assert(!LinkManager::hasInstance());
  LinkManager::_instance = new LinkManager;

  bool init_ok = true;

    // Split the line into an array of section names
    // GLOBAL/LINKS=Link1,Link2,Link3,...
  vector<string> linklist;
  SvxLink::splitStr(linklist, link_names, ",");

    // Loop to create a link object for each single linkdefinition
    // in the "LINKS=Link1,Link2,Link3" set
  for (vector<string>::const_iterator name_it = linklist.begin();
       name_it != linklist.end();
       name_it++)
  {
    if (LinkManager::instance()->links.find(*name_it) !=
        LinkManager::instance()->links.end())
    {
      continue;
    }

      // This assignment will create a new link definition
    Link &link = LinkManager::instance()->links[*name_it];

      // The link name is the same as the config section name
    link.name = *name_it;
    LinkManager::instance()->link_order.push_back(&link);

      // Logic1:70:name1,Logic2:71:name2,...
    string connect_logics;
    cfg.getValue(link.name, "CONNECT_LOGICS", connect_logics);

    vector<string> logic_specs;
    SvxLink::splitStr(logic_specs, connect_logics, ",");

      // Configure the list of logics to connect
      // CONNECT_LOGICS=Logic1:70:Name1,Logic2:71,Logic3:72:Name2,...
    for(vector<string>::iterator it = logic_specs.begin();
        it != logic_specs.end();
        ++it)
    {
      vector<string> logic_spec;
      SvxLink::splitStr(logic_spec, *it, ":");
      logic_spec.resize(3);
      string logic_name = logic_spec.at(0);
      if (!logic_name.empty())
      {
        LogicProperties logic_props;
        logic_props.cmd =  logic_spec.at(1);
        logic_props.announcement_name = logic_spec.at(2);
        pair<LogicPropMap::iterator, bool> ret =
            link.logic_props.insert(make_pair(logic_name, logic_props));
        if (!ret.second)
        {
          cerr << "*** ERROR: Duplicate logic \"" << logic_name
               << "\" specified in link \"" << link.name << "\".\n";
          init_ok = false;
        }
      }
      else
      {
        cerr << "*** ERROR: Bad configuration for " << link.name
             << "/CONNECT_LOGICS=" << connect_logics << ". Legal format: "
             << "<logic name>:<command>:<announcement name>,...\n";
        init_ok = false;
      }
    }

    if (link.logic_props.size() < 2)
    {
      cerr << "*** ERROR: " << link.name << "/CONNECT_LOGICS=? You must "
           << "have at least two logics to connect.\n";
      init_ok = false;
    }

    int timeout = -1;
    cfg.getValue(link.name, "TIMEOUT", timeout);

      // Automatically activate the link, if one (or more) logics
      // has activity, e.g. squelch open, announcement activity etc.
    string activate_on_activity;
    if (cfg.getValue(link.name, "AUTOACTIVATE_ON_SQL", activate_on_activity))
    {
      std::cerr << "*** WARNING: Configuration variable " << link.name
                << "/AUTOACTIVATE_ON_SQL has been renamed to "
                   "ACTIVATE_ON_ACTIVITY"
                << std::endl;
      cfg.setValue(link.name, "ACTIVATE_ON_ACTIVITY", activate_on_activity);
    }
    if (cfg.getValue(link.name, "ACTIVATE_ON_ACTIVITY", activate_on_activity))
    {
      SvxLink::splitStr(link.auto_activate, activate_on_activity, ",");

        // An automatically connected link should be disconnected after a
        // while so the TIMEOUT configuration variable must be set.
      if (timeout <= 0)
      {
        std::cerr << "*** WARNING: missing param " << link.name
                  << "/TIMEOUT=??, set to default (30 sec)" << std::endl;
        timeout = 30;
      }
    }

    if (cfg.getValue(link.name, "ACTIVATE_ON_TG", link.auto_activate_on_tg))
    {
        // An automatically connected link should be disconnected after a
        // while so the TIMEOUT configuration variable must be set.
      if (timeout <= 0)
      {
        std::cerr << "*** WARNING: Missing configuration " << link.name
                  << "/TIMEOUT=??, setting to default (30 sec)" << std::endl;
        timeout = 30;
      }

        // Validate and precompile the ACTIVATE_ON_TG regex patterns here so
        // that an invalid pattern is caught at configuration time instead of
        // crashing the application when the first TG update event arrives,
        // and so that the pattern is not recompiled on every TG update.
      for (const auto &item : link.auto_activate_on_tg)
      {
        try
        {
          link.auto_activate_on_tg_regex.emplace(item.first,
                                                 std::regex(item.second));
        }
        catch (const std::regex_error &e)
        {
          std::cerr << "*** ERROR: Invalid ACTIVATE_ON_TG pattern \""
                    << item.second << "\" for logic \"" << item.first
                    << "\" in link \"" << link.name << "\": " << e.what()
                    << std::endl;
          init_ok = false;
        }
      }
    }

    if (timeout > 0)
    {
      link.timeout_timer = new Timer(1000 * timeout);
      link.timeout_timer->setEnable(false);
      link.timeout_timer->expired.connect(sigc::bind(
          mem_fun(*LinkManager::instance(), &LinkManager::linkTimeout),
          &link));
    }

    cfg.getValue(link.name, "DEFAULT_ACTIVE", link.default_active);

      // Parse AUDIO_MODE configuration (default: FIRST)
    std::string audio_mode_str;
    if (cfg.getValue(link.name, "AUDIO_MODE", audio_mode_str))
    {
      if (audio_mode_str == "FIRST")
      {
        link.audio_mode = LinkAudioMode::FIRST;
      }
      else if (audio_mode_str == "MIX")
      {
        link.audio_mode = LinkAudioMode::MIX;
      }
      else if (audio_mode_str == "DUCK")
      {
        link.audio_mode = LinkAudioMode::DUCK;
      }
      else if (audio_mode_str == "PRIORITY")
      {
        link.audio_mode = LinkAudioMode::PRIORITY;
      }
      else
      {
        std::cerr << "*** WARNING: Unknown AUDIO_MODE \"" << audio_mode_str
                  << "\" in link " << link.name
                  << ". Valid options: FIRST, MIX, DUCK, PRIORITY. Using FIRST."
                  << std::endl;
      }
    }

      // Parse PRIORITY_SOURCES configuration (default: all members). Only
      // traffic from the listed members pre-empts other audio. The other
      // members of the link only act as sinks for the priority traffic.
    std::string priority_sources_str;
    if (cfg.getValue(link.name, "PRIORITY_SOURCES", priority_sources_str))
    {
      link.priority_sources_set = true;
      std::vector<std::string> priority_sources;
      SvxLink::splitStr(priority_sources, priority_sources_str, ",");
      for (const auto& logic_name : priority_sources)
      {
        if (link.logic_props.count(logic_name) == 0)
        {
          std::cerr << "*** WARNING: Logic \"" << logic_name
                    << "\" in " << link.name << "/PRIORITY_SOURCES is not "
                       "specified in " << link.name << "/CONNECT_LOGICS. "
                       "Ignoring it." << std::endl;
          continue;
        }
        link.priority_sources.insert(logic_name);
      }
      if (link.audio_mode != LinkAudioMode::PRIORITY)
      {
        std::cerr << "*** WARNING: " << link.name << "/PRIORITY_SOURCES is "
                     "only used with AUDIO_MODE=PRIORITY. Ignoring it."
                  << std::endl;
      }
      else if (link.priority_sources.empty())
      {
        std::cerr << "*** WARNING: " << link.name << "/PRIORITY_SOURCES "
                     "does not name any member of the link, so no traffic on "
                     "the link will pre-empt other audio." << std::endl;
      }
    }

      // DUCK and PRIORITY both key off a member logic's local squelch state
      // (LogicBase::squelchStateChanged), which is only ever emitted by the
      // Logic base class (Simplex/Repeater logics). Every other logic core
      // type (e.g. Reflector, ReflectorV2, Dummy, or a plugin such as Sip or
      // Announce) derives directly from LogicBase and never emits it, so a
      // member of such a type can never be the "local" side of a
      // DUCK/PRIORITY link - warn at config time rather than silently never
      // ducking/preempting.
    if ((link.audio_mode == LinkAudioMode::DUCK) ||
        (link.audio_mode == LinkAudioMode::PRIORITY))
    {
      for (const auto& logic_prop : link.logic_props)
      {
        const std::string& logic_name = logic_prop.first;
          // In a PRIORITY link, only the traffic of the priority sources
          // matters. The other members are only sinks.
        if ((link.audio_mode == LinkAudioMode::PRIORITY) &&
            !link.isPrioritySource(logic_name))
        {
          continue;
        }
        std::string logic_type;
        cfg.getValue(logic_name, "TYPE", logic_type);
        if ((logic_type != "Simplex") && (logic_type != "Repeater"))
        {
          std::cerr << "*** WARNING: Link " << link.name << " uses AUDIO_MODE="
                    << ((link.audio_mode == LinkAudioMode::DUCK) ?
                        "DUCK" : "PRIORITY")
                    << " but member logic \"" << logic_name
                    << "\" is TYPE=" << logic_type << ", which never reports "
                       "a local squelch state. DUCK/PRIORITY audio "
                       "ducking/preemption keyed off this logic's own "
                       "traffic will never trigger; see the AUDIO_MODE "
                       "documentation in svxlink.conf(5) for details."
                    << std::endl;
        }
      }
    }

      // Parse DUCK_LEVEL_DB configuration (default: -12 dB)
    cfg.getValue(link.name, "DUCK_LEVEL_DB", link.duck_level_db);

      // Parse PRIORITY_MUTE_DB configuration (default: -60 dB)
    cfg.getValue(link.name, "PRIORITY_MUTE_DB", link.priority_mute_db);

      // Parse PRIORITY_HANGTIME configuration (default: 0 = disabled)
    cfg.getValue(link.name, "PRIORITY_HANGTIME", link.priority_hangtime);
    if (link.priority_hangtime > 0)
    {
      link.priority_hangtime_timer = new Timer(link.priority_hangtime);
      link.priority_hangtime_timer->setEnable(false);
      link.priority_hangtime_timer->expired.connect(sigc::bind(
          mem_fun(*LinkManager::instance(),
                  &LinkManager::onPriorityHangtimeExpired),
          &link));
    }
  }

  if(!init_ok)
  {
    deleteInstance();
  }

  return init_ok;

} /* LinkManager::initialize */


void LinkManager::addLogic(LogicBase *logic)
{
    // Make sure that we have not added this logic before
  assert(logic_map.find(logic->name()) == logic_map.end());

    // Make sure that the logic connection objects have been created
  assert(logic->logicConOut() != 0);
  assert(logic->logicConIn() != 0);

    // Create a splitter to split the source audio from the logic being added
    // to all other logics.
  AudioSplitter *splitter = new AudioSplitter;
  logic->logicConOut()->registerSink(splitter);

    // Register the new logic source
  sources[logic->name()].source = logic->logicConOut();
  sources[logic->name()].splitter = splitter;

    // Create a selector for the logic being added to receive audio from all
    // other sinks. Used in FIRST mode.
  LogicInFeed *feed = new LogicInFeed;
  feed->registerSink(logic->logicConIn());
  AudioSelector *selector = new AudioSelector;
  selector->registerSink(feed);

    // Create a mixer for MIX mode. The mixer output will be connected
    // to the logic input when MIX mode connections are activated.
  AudioMixer *mixer = new AudioMixer;

    // Create a passthrough that lets the selector output feed the mixer as one
    // more source. When the mixer drives the logic input (any MIX/DUCK/PRIORITY
    // source active), the selector is routed here instead of being disconnected
    // so FIRST-mode audio is mixed in rather than discarded. It is a permanent
    // mixer source; the selector is (dis)connected from its input dynamically
    // in updateMixerRouting.
  AudioPassthrough *selector_feed = new AudioPassthrough;
  mixer->addSource(selector_feed);

    // Register the new logic sink
  sinks[logic->name()].sink = logic->logicConIn();
  sinks[logic->name()].feed = feed;
  sinks[logic->name()].selector = selector;
  sinks[logic->name()].mixer = mixer;
  sinks[logic->name()].selector_feed = selector_feed;

    // Now create a connection from the new logic source to each sink.
  for (SinkMap::iterator it=sinks.begin(); it != sinks.end(); ++it)
  {
      // A logic's source is never connected to its own sink (a logic must not
      // hear itself), so skip the self entry. The loop below over the source
      // map skips it as well, so sinks[name].connectors[name] is intentionally
      // never created; deleteLogic tolerates its absence.
    if ((*it).first == logic->name())
    {
      continue;
    }
    AudioPassthrough *connector = new AudioPassthrough;
    splitter->addSink(connector, true);
    AudioSelector *other_selector = (*it).second.selector;
    other_selector->addSource(connector);
    (*it).second.connectors[logic->name()] = connector;

      // Valve + Amp for MIX/DUCK mode (mixer path)
    AudioValve *valve = new AudioValve;
    valve->setOpen(false);  // Initially closed
    splitter->addSink(valve, true);
    AudioAmp *amp = new AudioAmp;
    amp->setGain(0);  // Normal gain initially
    valve->registerSink(amp, true);
    (*it).second.mixer->addSource(amp);
    (*it).second.valves[logic->name()] = valve;
    (*it).second.amps[logic->name()] = amp;
  }

    // Now create a connection from each existing logic source to the new sink.
    // Skip the newly-added logic itself: a logic's source is never connected to
    // its own sink (it must not hear itself), so there is no self-connection.
  for (SourceMap::iterator it=sources.begin(); it!=sources.end(); ++it)
  {
    if (it->first == logic->name()) continue;

      // Passthrough for FIRST mode (selector path)
    AudioPassthrough *connector = new AudioPassthrough;
    (*it).second.splitter->addSink(connector, true);
    selector->addSource(connector);
    sinks[logic->name()].connectors[(*it).first] = connector;

      // Valve + Amp for MIX/DUCK mode (mixer path)
    AudioValve *valve = new AudioValve;
    valve->setOpen(false);  // Initially closed
    (*it).second.splitter->addSink(valve, true);
    AudioAmp *amp = new AudioAmp;
    amp->setGain(0);  // Normal gain initially
    valve->registerSink(amp, true);
    mixer->addSource(amp);
    sinks[logic->name()].valves[(*it).first] = valve;
    sinks[logic->name()].amps[(*it).first] = amp;
  }

    // Create new object containing metadata for this logic core
  LogicInfo logic_info(logic);

    // Keep track of the newly added logics idle state so that we can start
    // and stop timeout timers.
  logic_info.idle_state_changed_con = logic->idleStateChanged.connect(
      sigc::bind(mem_fun(*this, &LinkManager::logicIdleStateChanged), logic));

    // Connect signals that convey information to other linked logics
  logic_info.received_tg_update_con = logic->receivedTgUpdated.connect(
      sigc::bind<0>(
        sigc::mem_fun(*this, &LinkManager::onReceivedTgUpdated), logic));
  logic_info.received_publish_state_event_con = logic->publishStateEvent.connect(
      sigc::bind<0>(
        sigc::mem_fun(*this, &LinkManager::onPublishStateEvent), logic));

    // Connect squelch state signal for DUCK mode
  logic_info.squelch_state_changed_con = logic->squelchStateChanged.connect(
      sigc::bind(
        sigc::mem_fun(*this, &LinkManager::onSquelchStateChanged), logic));

    // Add the logic core to the logic map
  logic_map.emplace(logic->name(), logic_info);

    // Create command objects associated with this logic
    // FIXME: We should not reference to a specific logic core type in this
    //        class
  Logic *cmd_logic = dynamic_cast<Logic*>(logic);
  if (cmd_logic != 0)
  {
    for (LinkMap::iterator it = links.begin(); it != links.end(); ++it)
    {
      Link &link = it->second;
      LogicPropMap::const_iterator prop_it(link.logic_props.find(logic->name()));
      if (prop_it != link.logic_props.end())
      {
        const LogicProperties &logic_props = prop_it->second;
        if (atoi(logic_props.cmd.c_str()) > 0)
        {
          LinkCmd *link_cmd = new LinkCmd(cmd_logic, link);
          if (!link_cmd->initialize(logic_props.cmd))
          {
            std::cerr << "*** WARNING: Can not setup command "
                      << logic_props.cmd << " for the logic " << logic->name()
                      << std::endl;
          }
        }
      }
    }
  }
} /* LinkManager::addLogic */


void LinkManager::deleteLogic(LogicBase *logic)
{
  LogicMap::iterator lmit = logic_map.find(logic->name());
  if (lmit == logic_map.end())
  {
    return;
  }

  LogicInfo &logic_info = (*lmit).second;
  assert(logic_info.logic == logic);
  assert(sources.find(logic->name()) != sources.end());
  assert(sinks.find(logic->name()) != sinks.end());

    // Disconnect the sigc signals that was connected when the
    // logic was first registered.
  assert(logic_info.idle_state_changed_con.connected());
  logic_info.idle_state_changed_con.disconnect();
  assert(logic_info.received_tg_update_con.connected());
  logic_info.received_tg_update_con.disconnect();
  assert(logic_info.received_publish_state_event_con.connected());
  logic_info.received_publish_state_event_con.disconnect();
  if (logic_info.squelch_state_changed_con.connected())
  {
    logic_info.squelch_state_changed_con.disconnect();
  }

    // Delete the logic source splitter and all connections associated with it
  AudioSplitter *splitter = sources[logic->name()].splitter;
  for (SinkMap::iterator smit=sinks.begin(); smit!=sinks.end(); ++smit)
  {
    SinkInfo &sink_info = (*smit).second;

      // Remove passthrough from selector. A logic's source is never wired to
      // its own sink (addLogic skips the self-connection), so the logic's own
      // sink has no connector for it; skip it rather than assuming one exists.
    ConMap::iterator cmit = sink_info.connectors.find(logic->name());
    if (cmit != sink_info.connectors.end())
    {
      AudioPassthrough *connector = (*cmit).second;
      sink_info.selector->removeSource(connector);
      sink_info.connectors.erase(cmit);
      splitter->removeSink(connector);
      //delete connector;
    }

      // Close and remove valve and amp for mixer
    ValveMap::iterator vmit = sink_info.valves.find(logic->name());
    if (vmit != sink_info.valves.end())
    {
        // Reap the mixer's wrapper for this amp before the amp itself is
        // destroyed below. Without this, sink_info.mixer (which belongs to
        // a logic that is NOT being deleted and so survives this call)
        // would keep an orphaned MixerSrc pointing at a soon-to-be-deleted
        // amp forever, since AudioMixer has no other way to reap it.
      AmpMap::iterator amit = sink_info.amps.find(logic->name());
      if (amit != sink_info.amps.end())
      {
        sink_info.mixer->removeSource(amit->second);
      }

      vmit->second->setOpen(false);
        // removeSink deletes the valve, since it was added as a managed sink
        // of the splitter; that in turn deletes the valve's managed amp.
      splitter->removeSink(vmit->second);
      sink_info.valves.erase(vmit);
    }

      // Forget the amp for this connection. The amp must NOT be deleted here:
      // it is a managed sink of the valve above (valve->registerSink(amp,
      // true)), so it was already destroyed when the valve was deleted.
    sink_info.amps.erase(logic->name());
  }
  delete splitter;
  sources.erase(logic->name());

    // Delete the logic sink and all connections associated with it
  SinkInfo &sink_to_delete = sinks[logic->name()];
  AudioSelector *selector = sink_to_delete.selector;
  AudioMixer *mixer = sink_to_delete.mixer;

    // Clean up connectors (passthroughs for selector)
  ConMap &cons = sink_to_delete.connectors;
  for (ConMap::iterator cmit = cons.begin(); cmit != cons.end(); ++cmit)
  {
    AudioPassthrough *connector = (*cmit).second;
    selector->removeSource(connector);
    const string &source_name = (*cmit).first;
    sources[source_name].splitter->removeSink(connector);
    //delete connector;
  }

    // Clean up valves (for mixer). Deleting a valve also deletes its amp,
    // which is registered as a managed sink of the valve, so the amps must
    // not be deleted separately below.
  ValveMap &valves = sink_to_delete.valves;
  for (ValveMap::iterator vmit = valves.begin(); vmit != valves.end(); ++vmit)
  {
    AudioValve *valve = vmit->second;
    valve->setOpen(false);
    const string &source_name = vmit->first;
      // removeSink deletes the valve (managed sink of the splitter) and, with
      // it, the valve's managed amp.
    sources[source_name].splitter->removeSink(valve);
  }

  delete selector;
  delete mixer;
    // The selector_feed passthrough is a permanent source of the mixer but is
    // owned by us (the mixer only owns its MixerSrc wrapper, which was just
    // destroyed with the mixer). Deleting the selector above already detached
    // it as the selector's sink, so it now has no source or sink.
  delete sink_to_delete.selector_feed;
    // Both the selector and the mixer are gone, so nothing feeds the logic
    // input feed any more
  delete sink_to_delete.feed;
  sinks.erase(logic->name());

    // Finally remove the logic from the logic_map
  logic_map.erase(logic->name());

} /* LinkManager::deleteLogic */


void LinkManager::allLogicsStarted(void)
{
  all_logics_started = true;

  for (LinkMap::iterator it = links.begin(); it != links.end(); ++it)
  {
    Link &link = it->second;

      // Check that all logics that are specified for this link has been
      // registered
    LogicPropMap::iterator prop_it = link.logic_props.begin();
    while (prop_it != link.logic_props.end())
    {
      const string &logic_name = (*prop_it).first;
      if (logic_map.find(logic_name) == logic_map.end())
      {
        std::cerr << "*** WARNING: Logic " << logic_name
                  << " has been specified in logic link " << link.name
                  << " but that logic is missing. Removing logic from link."
                  << std::endl;
        LogicPropMap::iterator remove_it = prop_it++;
        link.logic_props.erase(remove_it);
      }
      else
      {
        ++prop_it;
      }
    }
  }

    // Now that all logic cores are registered, and the links only refer to
    // logic cores that exist, let the logic cores set up their initial
    // linking state (e.g. muting an offline logic core)
  for (auto& logic_item : logic_map)
  {
    logic_item.second.logic->allLogicsStarted();
  }

  for (LinkMap::iterator it = links.begin(); it != links.end(); ++it)
  {
    Link &link = it->second;

      // If default active, activate the link
    if (link.default_active)
    {
      activateLink(link, "DEFAULT_ACTIVE");
    }
  }
} /* LinkManager::allLogicsStarted */


string LinkManager::cmdReceived(LinkRef link, LogicBase *logic,
                                const string &subcmd)
{
  /* cout << "### LinkManager::cmdReceived: link=" << link.name
       << " logic=" << logic->name()
       << " subcmd=" << subcmd << endl;
  */

  stringstream ss;

  LogicPropMap::iterator prop_it = link.logic_props.find(logic->name());
  assert(prop_it != link.logic_props.end());
  LogicProperties &logic_props = prop_it->second;
  if (subcmd == "") // Disconnecting Link1 <-X-> Link2
  {
    if (!link.is_activated)
    {
      ss << "link_not_active \"";
    }
    else
    {
      deactivateLink(link);
      ss << "deactivating_link \"";
    }
    ss << logic_props.announcement_name << "\"";
  }
  else //if (subcmd == "1") // Connecting Logic1 <---> Logic2 (two ways)
  {
    if (!link.is_activated)
    {
      activateLink(link, "DTMF command");
      ss << "activating_link \"";
      ss << logic_props.announcement_name + "\"";
    }
    //else
    //{
    //  ss << "link_already_active \"";
    //}
    //ss << logic_props.announcement_name + "\"";
    sendCmdToLogics(link, logic, subcmd);
  }
  //else
  //{
  //  ss << "unknown_command " << logic_props.cmd << subcmd;
  //}
  return ss.str();
} /* LinkManager::cmdReceived */


LogicBase *LinkManager::currentTalkerFor(const std::string& logic_name)
{
  SinkInfo& sink = sinks[logic_name];

    // In MIX mode, there's no single current talker
  if (sink.mixer->isRegistered())
  {
    return nullptr;
  }

    // FIRST mode - find the selected source
  AudioSource *selected = sink.selector->selectedSource();
  const ConMap& con_map = sink.connectors;
  for (ConMap::const_iterator it = con_map.begin(); it != con_map.end(); ++it)
  {
    if (it->second == selected)
    {
      //cout << "### Selected source: " << it->first << endl;
      return logic_map.at(it->first).logic;
    }
  }
  return nullptr;
} /* LinkManager::currentTalkerFor */


LogicBase *LinkManager::findLogic(const std::string& logic_name)
{
  LogicMap::iterator it = logic_map.find(logic_name);
  if (it == logic_map.end())
  {
    return 0;
  }
  return it->second.logic;
} /* LinkManager::findLogic */


bool LinkManager::logicsAreLinked(const std::string& logic_name1,
                                  const std::string& logic_name2)
{
    // Two logics are considered linked when they are both members of at least
    // one currently-activated link. This reflects the configured/bridged
    // topology rather than instantaneous audio routing, which can change while
    // a module mutes logic linking.
  if (logic_name1 == logic_name2)
  {
    return false;
  }
  for (LinkMap::iterator it = links.begin(); it != links.end(); ++it)
  {
    Link &link = it->second;
    if (link.is_activated &&
        (link.logic_props.find(logic_name1) != link.logic_props.end()) &&
        (link.logic_props.find(logic_name2) != link.logic_props.end()))
    {
      return true;
    }
  }
  return false;
} /* LinkManager::logicsAreLinked */


bool LinkManager::logicsConnected(const std::string& logic_name1,
                                  const std::string& logic_name2) const
{
    // Two logics are connected when the link layer currently has an
    // established connection between them, in either direction. This is the
    // same for every audio mode, unlike the selector's auto-select state,
    // which only reflects FIRST-mode audio arbitration.
  return (current_cons.count(std::make_pair(logic_name1, logic_name2)) > 0) ||
         (current_cons.count(std::make_pair(logic_name2, logic_name1)) > 0);
} /* LinkManager::logicsConnected */


void LinkManager::setLogicMute(const LogicBase *logic, bool mute)
{
  LogicInfo &info = logic_map.at(logic->name());
  if (mute != info.is_muted)
  {
    info.is_muted = mute;
    updateConnections();
    for (const auto& link_name : getLinkNames(logic->name()))
    {
      checkTimeoutTimer(links.at(link_name));
    }
  }
} /* LinkManager::setLogicMute */


void LinkManager::playFile(LogicBase *src_logic, const std::string& path)
{
  const ConMap& con_map = sinks[src_logic->name()].connectors;
  for (ConMap::const_iterator it = con_map.begin(); it != con_map.end(); ++it)
  {
    const std::string& logic_name = it->first;
    LogicBase *logic = logic_map.at(logic_name).logic;
    if ((logic != src_logic) && logicsConnected(src_logic->name(), logic_name))
    {
      logic->playFile(path);
    }
  }
} /* LinkManager::playFile */


void LinkManager::playSilence(LogicBase *src_logic, int length)
{
  const ConMap& con_map = sinks[src_logic->name()].connectors;
  for (ConMap::const_iterator it = con_map.begin(); it != con_map.end(); ++it)
  {
    const std::string& logic_name = it->first;
    LogicBase *logic = logic_map.at(logic_name).logic;
    if ((logic != src_logic) && logicsConnected(src_logic->name(), logic_name))
    {
      logic->playSilence(length);
    }
  }
} /* LinkManager::playSilence */


void LinkManager::playTone(LogicBase *src_logic, int fq, int amp, int len)
{
  const ConMap& con_map = sinks[src_logic->name()].connectors;
  for (ConMap::const_iterator it = con_map.begin(); it != con_map.end(); ++it)
  {
    const std::string& logic_name = it->first;
    LogicBase *logic = logic_map.at(logic_name).logic;
    if ((logic != src_logic) && logicsConnected(src_logic->name(), logic_name))
    {
      logic->playTone(fq, amp, len);
    }
  }
} /* LinkManager::playTone */


void LinkManager::playDtmf(LogicBase *src_logic, const std::string& digits, int amp, int len)
{
  const ConMap& con_map = sinks[src_logic->name()].connectors;
  for (ConMap::const_iterator it = con_map.begin(); it != con_map.end(); ++it)
  {
    const std::string& logic_name = it->first;
    LogicBase *logic = logic_map.at(logic_name).logic;
    if ((logic != src_logic) && logicsConnected(src_logic->name(), logic_name))
    {
      logic->playDtmf(digits, amp, len);
    }
  }
} /* LinkManager::playDtmf */


void LinkManager::playFileAll(LogicBase *src_logic, const std::string& path)
{
    // Mirror the source's scheduled-announcement classification and forced
    // CTCSS state onto each target so CTCSS is keyed (or suppressed) the same
    // way on every port. Forcing CTCSS matters for deferred announcements that
    // interrupt active traffic: listeners filtering on CTCSS must still hear
    // them on every port.
    //
    // The forced-CTCSS flag is deliberately NOT restored after playFile: the
    // target only queues the audio here and plays it asynchronously, so the
    // flag must stay set for the whole playback (any updateTxCtcss recompute on
    // the target while it plays must still see it). The target clears it in its
    // own allMsgsWritten when the announcement drains. The scheduled flag, by
    // contrast, only selects the CTCSS category at the moment of the play call,
    // so it is saved and restored as before.
  const bool sched = src_logic->scheduledAnnouncement();
  const bool force = src_logic->forceCtcss();
  for (LogicMap::const_iterator it = logic_map.begin();
       it != logic_map.end(); ++it)
  {
    LogicBase *logic = it->second.logic;
    if ((logic != src_logic) && !it->second.is_muted &&
        !logic->announceAllExcluded())
    {
      const bool prev_sched = logic->scheduledAnnouncement();
      logic->setScheduledAnnouncement(sched);
        // Never lower the target's forceCtcss from the broadcast path: it may
        // be long-lived for the target's own still-draining deferred
        // announcement and is only ever cleared by the target's own
        // allMsgsWritten once its whole queue (including this mirrored
        // audio) has drained.
      if (force)
      {
        logic->setForceCtcss(true);
      }
        // A mirrored announcement must play immediately on the target, not be
        // captured by any scheduled-announcement deferral the target has open.
      logic->setDeferralSuppressed(true);
      logic->playFile(path);
      logic->setDeferralSuppressed(false);
      logic->setScheduledAnnouncement(prev_sched);
    }
  }
} /* LinkManager::playFileAll */


void LinkManager::playSilenceAll(LogicBase *src_logic, int length)
{
  const bool sched = src_logic->scheduledAnnouncement();
  const bool force = src_logic->forceCtcss();
  for (LogicMap::const_iterator it = logic_map.begin();
       it != logic_map.end(); ++it)
  {
    LogicBase *logic = it->second.logic;
    if ((logic != src_logic) && !it->second.is_muted &&
        !logic->announceAllExcluded())
    {
      const bool prev_sched = logic->scheduledAnnouncement();
        // forceCtcss is never lowered here; see playFileAll for the
        // rationale.
      logic->setScheduledAnnouncement(sched);
      if (force)
      {
        logic->setForceCtcss(true);
      }
      logic->setDeferralSuppressed(true);
      logic->playSilence(length);
      logic->setDeferralSuppressed(false);
      logic->setScheduledAnnouncement(prev_sched);
    }
  }
} /* LinkManager::playSilenceAll */


void LinkManager::playToneAll(LogicBase *src_logic, int fq, int amp, int len)
{
  const bool sched = src_logic->scheduledAnnouncement();
  const bool force = src_logic->forceCtcss();
  for (LogicMap::const_iterator it = logic_map.begin();
       it != logic_map.end(); ++it)
  {
    LogicBase *logic = it->second.logic;
    if ((logic != src_logic) && !it->second.is_muted &&
        !logic->announceAllExcluded())
    {
      const bool prev_sched = logic->scheduledAnnouncement();
        // forceCtcss is never lowered here; see playFileAll for the
        // rationale.
      logic->setScheduledAnnouncement(sched);
      if (force)
      {
        logic->setForceCtcss(true);
      }
      logic->setDeferralSuppressed(true);
      logic->playTone(fq, amp, len);
      logic->setDeferralSuppressed(false);
      logic->setScheduledAnnouncement(prev_sched);
    }
  }
} /* LinkManager::playToneAll */


void LinkManager::playDtmfAll(LogicBase *src_logic, const std::string& digits,
                              int amp, int len)
{
    // Nothing to play. Mirroring it anyway would raise the targets' forced
    // CTCSS flag without queueing anything whose completion clears it again.
  if (digits.empty())
  {
    return;
  }

  const bool sched = src_logic->scheduledAnnouncement();
  const bool force = src_logic->forceCtcss();
  for (LogicMap::const_iterator it = logic_map.begin();
       it != logic_map.end(); ++it)
  {
    LogicBase *logic = it->second.logic;
    if ((logic != src_logic) && !it->second.is_muted &&
        !logic->announceAllExcluded())
    {
      const bool prev_sched = logic->scheduledAnnouncement();
        // forceCtcss is never lowered here; see playFileAll for the
        // rationale.
      logic->setScheduledAnnouncement(sched);
      if (force)
      {
        logic->setForceCtcss(true);
      }
      logic->setDeferralSuppressed(true);
      logic->playDtmf(digits, amp, len);
      logic->setDeferralSuppressed(false);
      logic->setScheduledAnnouncement(prev_sched);
    }
  }
} /* LinkManager::playDtmfAll */


size_t LinkManager::sinkMixerSourceCount(const std::string& sink_name) const
{
  SinkMap::const_iterator sit = sinks.find(sink_name);
  if (sit == sinks.end())
  {
    return 0;
  }
  return sit->second.mixer->sourceCount();
} /* LinkManager::sinkMixerSourceCount */


bool LinkManager::linkValveOpen(const std::string& src_name,
                                const std::string& sink_name) const
{
  SinkMap::const_iterator sit = sinks.find(sink_name);
  if (sit == sinks.end())
  {
    return false;
  }
  ValveMap::const_iterator vit = sit->second.valves.find(src_name);
  if (vit == sit->second.valves.end())
  {
    return false;
  }
  return vit->second->isOpen();
} /* LinkManager::linkValveOpen */


float LinkManager::linkGain(const std::string& src_name,
                            const std::string& sink_name) const
{
  SinkMap::const_iterator sit = sinks.find(sink_name);
  if (sit == sinks.end())
  {
    return std::numeric_limits<float>::quiet_NaN();
  }
  AmpMap::const_iterator ait = sit->second.amps.find(src_name);
  if (ait == sit->second.amps.end())
  {
    return std::numeric_limits<float>::quiet_NaN();
  }
  return ait->second->gain();
} /* LinkManager::linkGain */


bool LinkManager::linkSelectorEnabled(const std::string& src_name,
                                      const std::string& sink_name) const
{
  SinkMap::const_iterator sit = sinks.find(sink_name);
  if (sit == sinks.end())
  {
    return false;
  }
  ConMap::const_iterator cit = sit->second.connectors.find(src_name);
  if (cit == sit->second.connectors.end())
  {
    return false;
  }
  return sit->second.selector->autoSelectEnabled(cit->second);
} /* LinkManager::linkSelectorEnabled */


bool LinkManager::sinkSelectorRoutedToMixer(const std::string& sink_name) const
{
  SinkMap::const_iterator sit = sinks.find(sink_name);
  if (sit == sinks.end())
  {
    return false;
  }
  return sit->second.selector->sink() == sit->second.selector_feed;
} /* LinkManager::sinkSelectorRoutedToMixer */


bool LinkManager::activateLinkByName(const std::string& name)
{
  LinkMap::iterator it = links.find(name);
  if (it == links.end())
  {
    return false;
  }
  activateLink(it->second, "activateLinkByName");
  return true;
} /* LinkManager::activateLinkByName */


bool LinkManager::deactivateLinkByName(const std::string& name)
{
  LinkMap::iterator it = links.find(name);
  if (it == links.end())
  {
    return false;
  }
  deactivateLink(it->second, "deactivateLinkByName");
  return true;
} /* LinkManager::deactivateLinkByName */



/****************************************************************************
 *
 * Protected member functions
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Private member functions
 *
 ****************************************************************************/

LinkManager::~LinkManager(void)
{
  LogicMap::iterator it = logic_map.begin();
  while (it != logic_map.end())
  {
    deleteLogic(it->second.logic);
    it = logic_map.begin();
  }
} /* LinkManager::~LinkManager */


/**
 * @brief Find out which logics that should be connected
 * @param want The connections we want
 *
 * This function will calculate which logic connections that should currently
 * be established to get the correct audio flow in the currently activated
 * logic links. It will not set up the logic connections though so the caller
 * have to figure out which currently active connections to break and which to
 * establish to get to the set of connections reported by this function.
 */
void LinkManager::wantedConnections(LogicConSet &want)
{
    // Fill "want" with the base set of connections that are explicitly
    // specified in the link configuration for active links. The connection
    // graph will not be complete if two links share one or more logics. This
    // will be solved in the next step below.
  for (LinkMap::iterator lit = links.begin(); lit != links.end(); ++lit)
  {
    Link &link = (*lit).second;
    if (link.is_activated)
    {
        // oit=outer iterator
      for (LogicPropMap::iterator oit = link.logic_props.begin();
           oit != link.logic_props.end();
           ++oit)
      {
          // iit=inner iterator
        LogicPropMap::iterator iit = oit;
        for (++iit; iit != link.logic_props.end(); ++iit)
        {
          const std::string& l1_name = (*oit).first;
          const std::string& l2_name = (*iit).first;
          const LogicInfo &l1_info = logic_map.at(l1_name);
          const LogicInfo &l2_info = logic_map.at(l2_name);
          if ((l1_name != l2_name) && !l1_info.is_muted && !l2_info.is_muted)
          {
            want.insert(make_pair(l1_name, l2_name));
            want.insert(make_pair(l2_name, l1_name));
          }
        }
      }
    }
  }

    // Now we need to complete the graph with connections between logics that
    // are implicitly connected. That is, logics that are connected to each
    // other via other logics.
    //
    // The algoritm below is not very optimized but that should not be needed
    // since SvxLink most often just have a few logics defined. If a lot of
    // logics is used, the code below may have to be optimized.
    //
    // The outer loop (do {} while) is needed since new connections may be
    // added and then new implicit connections may appear. We are done when
    // all connections have been gone through and no new connections was
    // added.
  size_t prev_size;
  do
  {
      // Store the current size of the connection set so that we can check
      // later if new connections have been added.
    prev_size = want.size();

      // Now compare all connections to each other. This requires two loops,
      // an outer (oit=outer iterator) and an inner (iit=inner iterator).
    for (LogicConSet::iterator oit = want.begin();
         oit != want.end();
         ++oit)
    {
      for (LogicConSet::iterator iit = want.begin();
           iit != want.end();
           ++iit)
      {
          // If the sink of the outer connection belongs to the same logic
          // as the source of the inner connection, then we need to also
          // connect the source of the outer connection to the sink of the
          // inner connection. To exemplify, if the set contains the
          // connections: <L1,L2>,<L2,L3> we also need to add <L1,L3> since
          // L1 is implicitly connected to L3 via L2.
          // The comparison below also make sure that a connection is not
          // added which connect back to the same logic. To exemplify this,
          // if the set contains the connections: <L1,L2>,<L2,L1> we should
          // not connect <L1,L1>.
        if ((oit->second == iit->first) && (oit->first != iit->second))
        {
          want.insert(make_pair(oit->first, iit->second));
        }
      }
    }
  } while (want.size() > prev_size);
} /* LinkManager::wantedConnections */


void LinkManager::updateConnections(void)
{
    // Get the wanted logic connections based on which links that are activated
  LogicConSet want;
  wantedConnections(want);

    // Calculate which of the current connections should be broken.
    // This is easily done with a simple difference set operation.
    // After this operation the "to_disconnect" variable will contain
    // the set of logic connections that have to be broken.
  LogicConSet to_disconnect;
  set_difference(current_cons.begin(), current_cons.end(),
                 want.begin(), want.end(),
                 inserter(to_disconnect, to_disconnect.end()));

  for (auto it = to_disconnect.begin(); it != to_disconnect.end(); ++it)
  {
    const string &src_name = it->first;
    const string &sink_name = it->second;
    SinkInfo &sink = sinks.at(sink_name);

      // Disconnect the audio path from source logic to sink logic
    sink.selector->disableAutoSelect(sink.connectors.at(src_name));

      // Close the mixer valve for this connection
    sink.valves.at(src_name)->setOpen(false);

      // Reset the amp to unity gain. Without this, a stale DUCK_LEVEL_DB or
      // PRIORITY_MUTE_DB gain left over from before this link was
      // deactivated would otherwise survive a later reactivation of the
      // link (updateDuckingForSink/updatePriorityForSink only rewrite the
      // gain for connections whose *current* effective mode calls for it,
      // so a connection that drops out of DUCK/PRIORITY mode here is never
      // touched again until this reset).
    sink.amps.at(src_name)->setGain(0);

      // Delete the link connect information
    current_cons.erase(*it);
  }

    // Calculate the difference between the wanted connection set and the
    // current connection set. This is easily done using the difference set
    // operation. After this operation the "to_connect" variable will contain
    // the set of logic connections that have to be established.
  LogicConSet to_connect;
  set_difference(want.begin(), want.end(),
                 current_cons.begin(), current_cons.end(),
                 inserter(to_connect, to_connect.end()));

    // Record the newly-wanted connections. The actual selector/valve wiring is
    // done by the reconciliation loop below, which handles both new and
    // already-established connections uniformly.
  for (auto it = to_connect.begin(); it != to_connect.end(); ++it)
  {
    const string &src_name = it->first;
    const string &sink_name = it->second;

      // Reset to unity gain before the connection is (re-)established: a
      // freshly (re)activated connection must never inherit a stale
      // DUCK_LEVEL_DB/PRIORITY_MUTE_DB gain left on this amp by a previous
      // activation. The recompute loop below then applies whatever gain is
      // actually appropriate for the current squelch/priority state.
    sinks.at(sink_name).amps.at(src_name)->setGain(0);

    current_cons.insert(*it);
  }

    // The set of activated links has changed, so find out which links create
    // each connection. That decides the audio mode of the connection.
  updateConnectionLinks();

    // Reconcile the effective audio mode for EVERY established connection, not
    // just the ones added above. Two overlapping links can change the effective
    // mode of a connection that stays wanted (e.g. activating a MIX link over a
    // FIRST link, or a link that chains a PRIORITY link to the connection)
    // without adding or removing it from current_cons, so the selector/valve
    // path has to be recomputed here as well. All of the operations below are
    // idempotent.
  for (const auto &con : current_cons)
  {
    const string &src_name = con.first;
    const string &sink_name = con.second;
    SinkInfo &sink = sinks.at(sink_name);

    LinkAudioMode mode = getEffectiveMode(src_name, sink_name);

    if (mode == LinkAudioMode::FIRST)
    {
        // FIRST mode: use selector, close mixer valve
      sink.selector->enableAutoSelect(sink.connectors.at(src_name), 0);
      sink.valves.at(src_name)->setOpen(false);
    }
    else // MIX, DUCK, or PRIORITY mode - all use mixer
    {
        // MIX/DUCK/PRIORITY mode: disable selector branch, open mixer valve
      sink.selector->disableAutoSelect(sink.connectors.at(src_name));
      sink.valves.at(src_name)->setOpen(true);
    }
  }

    // Update mixer routing based on active MIX/DUCK/PRIORITY connections
  updateMixerRouting();

    // The set of established connections may have changed, which changes
    // which priority sources count as active, so update the priority
    // hangtime state first (e.g. a PRIORITY link activated while its source
    // is already transmitting must get a hangtime when the source unkeys).
  checkPriorityHangtime();

    // Recompute duck/priority gain and preemption for every sink, using the
    // *current* squelch/priority state. This ensures a sink whose connection
    // just became active picks up the correct gain immediately (e.g.
    // reactivating a DUCK link while the sink's squelch happens to already
    // be open) instead of waiting for the next squelch transition to notice.
    // It must cover all sinks, not only those whose connections changed
    // above, since the reconciliation loop re-enables FIRST-mode auto-select
    // on every established connection, including ones that a PRIORITY
    // source is currently preempting.
  for (const auto &sink_pair : sinks)
  {
    const std::string& sink_name = sink_pair.first;
    bool squelch_open = false;
    LogicMap::iterator logic_it = logic_map.find(sink_name);
    if (logic_it != logic_map.end())
    {
      squelch_open = logic_it->second.squelch_open;
    }
    updateDuckingForSink(sink_name, squelch_open);
    updatePriorityForSink(sink_name);
  }
} /* LinkManager::updateConnections */


void LinkManager::activateLink(Link &link, const std::string& reason)
{
  if (!link.is_activated)
  {
    std::cout << "Activating link '" << link.name << "'";
    if (!reason.empty())
    {
      std::cout << " due to " << reason;
    }
    std::cout << std::endl;
    link.is_activated = true;
    updateConnections();
    checkTimeoutTimer(link);
  }
} /* LinkManager::activateLink */


void LinkManager::deactivateLink(Link &link, const std::string& reason)
{
  if (link.is_activated)
  {
    std::cout << "Deactivating link " << link.name;
    if (!reason.empty())
    {
      std::cout << " due to " << reason;
    }
    std::cout << std::endl;
    link.is_activated = false;

      // Reset any priority hangtime state. checkPriorityHangtime skips
      // deactivated links, so a still-running timer (and the remembered set of
      // preempted sinks) would otherwise be frozen and resume a spurious mute
      // when the link is reactivated. Stop the timer, remember which sinks were
      // held muted, clear the state, then refresh those sinks below.
    std::set<std::string> was_muted;
    if (link.priority_hangtime_timer != nullptr)
    {
      link.priority_hangtime_timer->setEnable(false);
    }
    was_muted.swap(link.priority_active_sinks);

    updateConnections();
    checkTimeoutTimer(link);

      // Recompute priority routing for the sinks whose hangtime we just
      // cleared so any lingering mute gain is released.
    for (const auto& sink_name : was_muted)
    {
      updatePriorityForSink(sink_name);
    }
  }
} /* LinkManager::deactivateLink */


void LinkManager::sendCmdToLogics(Link& link, LogicBase* src_logic,
                                  const std::string& cmd)
{
  for (const auto& props : link.logic_props)
  {
    const string &logic_name = props.first;
    const LogicInfo &logic_info = logic_map.at(logic_name);
    if ((logic_info.logic != src_logic) && !logic_info.is_muted)
    {
      logic_info.logic->remoteCmdReceived(src_logic, cmd);
    }
  }
} /* LinkManager::sendCmdToLogics */


#if 0
bool LinkManager::isConnected(const string& source_name,
      	                      const string& sink_name)
{
  assert(sources.count(source_name) == 1);
  assert(sinks.count(sink_name) == 1);
  assert(sinks[sink_name].connectors.count(source_name) == 1);

  AudioPassthrough *connector = sinks[sink_name].connectors[source_name];
  AudioSelector *selector = sinks[sink_name].selector;
  return selector->autoSelectEnabled(connector);
} /* LinkManager::isConnected */
#endif


/**
 * @brief Returns all linknames in that the specific logic is involved
 */
vector<string> LinkManager::getLinkNames(const string& logicname)
{
  vector<string> linknames;
  for (LinkMap::iterator it=links.begin(); it!=links.end(); ++it)
  {
    Link &link = (*it).second;
    if (link.logic_props.find(logicname) != link.logic_props.end())
    {
      linknames.push_back(link.name);
    }
  }
  return linknames;
} /* LinkManager::getLinkNames */


void LinkManager::linkTimeout(Async::Timer *t, Link *link)
{
  if (link->is_activated && !link->default_active)
  {
    deactivateLink(*link);
  }

  if (!link->is_activated && link->default_active)
  {
    activateLink(*link, "TIMEOUT on default activated link");
  }
} /* LinkManager::linkTimeout */


void LinkManager::logicIdleStateChanged(bool is_idle, const LogicBase *logic)
{
  /* cout << "### LinkManager::logicIdleStateChanged:"
       << " is_idle=" << is_idle
       << " logic_name=" << logic->name()
       << endl;
  */

  if (!all_logics_started)
  {
    return;
  }

    // We need all linknames where the logic is included
  vector<string> link_names = getLinkNames(logic->name());

    // Loop through all links associated with the logic to see if we should
    // enable or disable any timeout timers.
  for (std::vector<string>::iterator lnit = link_names.begin();
       lnit != link_names.end();
       ++lnit)
  {
    const string &link_name = *lnit;
    LinkMap::iterator lmit = links.find(link_name);
    assert(lmit != links.end());
    Link &link = (*lmit).second;
    
      // Check if the logic that updated its idle state is defined as a
      // "auto_activate" logic. If not idle, all logics of the link shall
      // be connected if they were disconnected before.
      // FIXME: We now activate the link even on announcements since that
      // changes the idle status of a logic core. Probably not what we want.
    if (!is_idle && !link.is_activated)
    {
      StrSet::iterator acit = link.auto_activate.find(logic->name());
      if (acit != link.auto_activate.end())
      {
        std::ostringstream ss;
        ss << "ACTIVATE_ON_ACTIVITY from logic '"
           << logic->name() << "'";
        activateLink(link, ss.str());
      }
    }

    checkTimeoutTimer(link);
  }
} /* LinkManager::logicIdleStateChanged */


void LinkManager::checkTimeoutTimer(Link &link)
{
  if (link.timeout_timer == 0)
  {
    return;
  }

  bool all_logics_idle = true;
  for (const auto& prop : link.logic_props)
  {
    const std::string &logic_name = prop.first;
    const LogicInfo &logic_info = logic_map.at(logic_name);
    all_logics_idle &= logic_info.is_muted || logic_info.logic->isIdle();
  }

  link.timeout_timer->setEnable(
      all_logics_idle && (link.is_activated != link.default_active));
} /* LinkManager::checkTimeoutTimer */


void LinkManager::onReceivedTgUpdated(LogicBase *src_logic, uint32_t tg)
{
  //cout << "### LinkManager::onReceivedTgUpdated: logic=" << src_logic->name()
  //     << "  tg=" << tg << endl;


    // Loop through all links associated with the logic to see if we should
    // auto activate any links
  for (auto& link_spec : links)
  {
    Link &link = link_spec.second;

#if 1
    std::ostringstream tgss;
    tgss << tg;
    auto aait = link.auto_activate_on_tg_regex.find(src_logic->name());
    if (!link.is_activated &&
        //(link.logic_props.find( != link.logic_props.end()) &&
        aait != link.auto_activate_on_tg_regex.end() &&
        std::regex_match(tgss.str(), aait->second))
    {
      std::ostringstream ss;
      ss << "ACTIVATE_ON_TG from logic '"
         << src_logic->name() << "'"
         << " which has activated talkgroup " << tgss.str();
      activateLink(link, ss.str());
    }
#endif
  }


  const ConMap& con_map = sinks[src_logic->name()].connectors;
  for (ConMap::const_iterator it = con_map.begin(); it != con_map.end(); ++it)
  {
    const std::string& logic_name = it->first;
    LogicBase *logic = logic_map.at(logic_name).logic;
    if ((logic != src_logic) && logicsConnected(src_logic->name(), logic_name))
    {
      logic->remoteReceivedTgUpdated(src_logic, tg);
    }
  }
} /* LinkManager::onReceivedTgUpdated */


void LinkManager::onPublishStateEvent(LogicBase *src_logic,
    const std::string& event_name, const std::string& msg)
{
  //cout << "### LinkManager::onPublishStateEvent: logic=" << src_logic->name()
  //     << "  event_name=" << event_name
  //     << "  msg=" << msg
  //     << endl;
    // "ModuleFwd:state <cmd_id> 0" means that a forwarded module is free
  bool is_fwd_module_free = false;
  if (event_name == "ModuleFwd:state")
  {
    std::istringstream is(msg);
    std::string cmd_id;
    int active = 1;
    is_fwd_module_free = (is >> cmd_id >> active) && (active == 0);
  }
  const ConMap& con_map = sinks[src_logic->name()].connectors;
  for (ConMap::const_iterator it = con_map.begin(); it != con_map.end(); ++it)
  {
    const std::string& logic_name = it->first;
    LogicBase *logic = logic_map.at(logic_name).logic;
    if ((logic != src_logic) && logicsConnected(src_logic->name(), logic_name))
    {
      logic->remoteReceivedPublishStateEvent(src_logic, event_name, msg);
    }
    else if ((logic != src_logic) && is_fwd_module_free &&
             (dynamic_cast<Logic*>(logic) != nullptr))
    {
        // A forwarded module (FORWARD_MODULE_CMD) went idle on the target
        // logic. Band ports that are no longer linked to the target (e.g. the
        // link was deactivated while the module was active) must still hear
        // that it is free, or they would keep reporting it busy forever.
        // Receiving the free event is harmless for a logic that never held
        // the module.
      logic->remoteReceivedPublishStateEvent(src_logic, event_name, msg);
    }
  }
} /* LinkManager::onPublishStateEvent */


void LinkManager::updateConnectionLinks(void)
{
    // Find out which activated links create each established connection. A
    // connection between two logics that are both members of one or more
    // activated links is created by those links. Otherwise, the connection
    // only exists because activated links are chained via common logics
    // (e.g. A-B in one link and B-C in another one creates A-C). It is then
    // created by every link that the audio can pass through on its way from
    // the source to the sink, that is the links that join two logics on some
    // path from the source to the sink that does not visit any logic twice.
  con_links.clear();

    // The graph of direct connections between unmuted logics, as set up by
    // wantedConnections
  BlockFinder::Graph graph;
  for (const Link* link : link_order)
  {
    if (!link->is_activated)
    {
      continue;
    }
    for (const auto& l1 : link->logic_props)
    {
      for (const auto& l2 : link->logic_props)
      {
        if ((l1.first != l2.first) && !logic_map.at(l1.first).is_muted &&
            !logic_map.at(l2.first).is_muted)
        {
          graph[l1.first].insert(l2.first);
        }
      }
    }
  }

    // The edges on the simple paths between two vertices are exactly the
    // edges of the blocks on the path between the two vertices in the tree
    // formed by the blocks and the vertices they contain. Set up that tree.
  BlockFinder finder(graph);
  const auto& blocks = finder.blocks();
  std::map<std::string, std::vector<size_t>> vertex_blocks;
  std::vector<std::set<std::string>> block_vertices(blocks.size());
  for (size_t i = 0; i < blocks.size(); ++i)
  {
    for (const auto& edge : blocks[i])
    {
      block_vertices[i].insert(edge.first);
      block_vertices[i].insert(edge.second);
    }
    for (const auto& vertex : block_vertices[i])
    {
      vertex_blocks[vertex].push_back(i);
    }
  }

  for (const auto& con : current_cons)
  {
    const std::string& src_name = con.first;
    const std::string& sink_name = con.second;
    LinkPtrVec& con_link_vec = con_links[con];

    for (const Link* link : link_order)
    {
      if (link->is_activated && (link->logic_props.count(src_name) > 0) &&
          (link->logic_props.count(sink_name) > 0))
      {
        con_link_vec.push_back(link);
      }
    }
    if (!con_link_vec.empty() || (vertex_blocks.count(src_name) == 0))
    {
      continue;
    }

      // An implicit connection. Search the tree from the source to the sink,
      // remembering how each block was reached.
    std::map<size_t, std::string> block_from;
    std::map<std::string, size_t> vertex_from;
    std::vector<std::string> queue(1, src_name);
    vertex_from[src_name] = blocks.size();
    for (size_t qi = 0; (qi < queue.size()) &&
                        (vertex_from.count(sink_name) == 0); ++qi)
    {
      const std::string vertex = queue[qi];
      for (size_t block : vertex_blocks[vertex])
      {
        if (block_from.count(block) > 0)
        {
          continue;
        }
        block_from[block] = vertex;
        for (const auto& next : block_vertices[block])
        {
          if (vertex_from.count(next) == 0)
          {
            vertex_from[next] = block;
            queue.push_back(next);
          }
        }
      }
    }
    if (vertex_from.count(sink_name) == 0)
    {
      continue;
    }
    std::set<LogicCon> path_edges;
    std::string vertex = sink_name;
    while (vertex != src_name)
    {
      size_t block = vertex_from[vertex];
      for (const auto& edge : blocks[block])
      {
        path_edges.insert(edge);
      }
      vertex = block_from[block];
    }
    for (const Link* link : link_order)
    {
      if (!link->is_activated)
      {
        continue;
      }
      for (const auto& edge : path_edges)
      {
        if ((link->logic_props.count(edge.first) > 0) &&
            (link->logic_props.count(edge.second) > 0))
        {
          con_link_vec.push_back(link);
          break;
        }
      }
    }
  }
} /* LinkManager::updateConnectionLinks */


const LinkManager::LinkPtrVec& LinkManager::connectionLinks(
    const std::string& src_name, const std::string& sink_name) const
{
  static const LinkPtrVec no_links;
  ConLinkMap::const_iterator it =
    con_links.find(std::make_pair(src_name, sink_name));
  if (it == con_links.end())
  {
    return no_links;
  }
  return it->second;
} /* LinkManager::connectionLinks */


const LinkManager::Link* LinkManager::modeLink(
    const std::string& src_name, const std::string& sink_name) const
{
    // The link that decides the audio mode of a connection, and the DUCK
    // parameters for it, is the one with the strongest mode of the links
    // that create the connection: PRIORITY > DUCK > MIX > FIRST. Among
    // links with the same mode, the one given first in GLOBAL/LINKS wins.
  const Link* mode_link = nullptr;
  for (const Link* link : connectionLinks(src_name, sink_name))
  {
    if ((mode_link == nullptr) ||
        (modeStrength(link->audio_mode) > modeStrength(mode_link->audio_mode)))
    {
      mode_link = link;
    }
  }
  return mode_link;
} /* LinkManager::modeLink */


const LinkManager::Link* LinkManager::priorityLink(
    const std::string& src_name, const std::string& sink_name) const
{
    // The PRIORITY link that makes the source pre-empt other audio into the
    // sink: the first one, in GLOBAL/LINKS order, of the PRIORITY links that
    // create the connection and have the source as a priority source
  for (const Link* link : connectionLinks(src_name, sink_name))
  {
    if ((link->audio_mode == LinkAudioMode::PRIORITY) &&
        link->isPrioritySource(src_name))
    {
      return link;
    }
  }
  return nullptr;
} /* LinkManager::priorityLink */


LinkAudioMode LinkManager::getEffectiveMode(const std::string& src_name,
                                             const std::string& sink_name)
{
  const Link* link = modeLink(src_name, sink_name);
  return (link != nullptr) ? link->audio_mode : LinkAudioMode::FIRST;
} /* LinkManager::getEffectiveMode */


void LinkManager::updateMixerRouting(void)
{
    // Connect/disconnect mixer to logic input based on active MIX connections.
    // If a sink has any open mixer valves, route the mixer output to the logic.
    // Otherwise, route the selector output to the logic.
  for (auto& sink_pair : sinks)
  {
    SinkInfo& sink = sink_pair.second;

      // Check if this sink has any MIX-mode connections active (any valve open)
    bool has_mix_connection = false;
    for (const auto& valve_pair : sink.valves)
    {
      if (valve_pair.second->isOpen())
      {
        has_mix_connection = true;
        break;
      }
    }

      // Route mixer or selector to logic input
    if (has_mix_connection)
    {
        // The mixer drives the logic input. Rather than disconnecting the
        // selector (which would silently discard any FIRST-mode source feeding
        // this sink), route the selector output into the mixer via selector_feed
        // so FIRST-mode arbitration is preserved among FIRST sources while the
        // MIX/DUCK/PRIORITY sources mix in.
      if (sink.selector->sink() != sink.selector_feed)
      {
        if (sink.selector->isRegistered())
        {
          sink.selector->unregisterSink();
        }
          // The selector may have been in the middle of a stream into the
          // logic input. End it here: the mixer only flushes a stream that it
          // has written to itself, so if the selector source stops before any
          // sample has passed through the mixer the end of the stream would
          // otherwise never reach the logic input.
        sink.feed->endStream();
        sink.selector->registerSink(sink.selector_feed);
      }
      if (!sink.mixer->isRegistered())
      {
        sink.mixer->registerSink(sink.feed);
      }
    }
    else
    {
        // No mixer sources: disconnect the mixer and drive the logic input
        // directly from the selector.
      if (sink.mixer->isRegistered())
      {
        sink.mixer->unregisterSink();
          // Unregistering does not flush the logic input, so if the mixer was
          // in the middle of a stream, end it here. Otherwise the logic input
          // never sees the end of the stream and the transmitter stays keyed.
        sink.feed->endStream();
      }
      if (sink.selector->sink() != sink.feed)
      {
        const bool was_feeding_mixer =
            (sink.selector->sink() == sink.selector_feed);
        if (sink.selector->isRegistered())
        {
          sink.selector->unregisterSink();
        }
          // If the selector was feeding the mixer, it may have been cut off
          // in the middle of a stream. Flush the feed so that the mixer does
          // not keep waiting for samples from it, which would stall every
          // other mixer source the next time the mixer is used.
        if (was_feeding_mixer)
        {
          sink.selector_feed->flushSamples();
        }
        sink.selector->registerSink(sink.feed);
      }
    }
  }
} /* LinkManager::updateMixerRouting */


void LinkManager::onSquelchStateChanged(bool is_open, LogicBase *logic)
{
    // Update squelch state tracking
  LogicMap::iterator it = logic_map.find(logic->name());
  if (it != logic_map.end())
  {
    it->second.squelch_open = is_open;
  }

    // Update ducking for this logic's incoming connections
  updateDuckingForSink(logic->name(), is_open);

    // Check if we need to start/stop priority hangtime timers
  checkPriorityHangtime();

    // Update priority preemption for all sinks
  for (auto& sink_pair : sinks)
  {
    updatePriorityForSink(sink_pair.first);
  }
} /* LinkManager::onSquelchStateChanged */


void LinkManager::updateDuckingForSink(const std::string& sink_name,
                                        bool local_squelch_open)
{
  SinkMap::iterator sink_it = sinks.find(sink_name);
  if (sink_it == sinks.end())
  {
    return;
  }

  SinkInfo& sink = sink_it->second;

  for (auto& amp_pair : sink.amps)
  {
    const std::string& src_name = amp_pair.first;
    AudioAmp* amp = amp_pair.second;

      // Check if this connection uses DUCK mode
    LinkAudioMode mode = getEffectiveMode(src_name, sink_name);

    if (mode == LinkAudioMode::DUCK)
    {
      if (local_squelch_open)
      {
          // Duck incoming audio when local squelch is open
        float duck_level = getEffectiveDuckLevel(src_name, sink_name);
        amp->setGain(duck_level);
      }
      else
      {
          // Restore normal gain when local squelch closes
        amp->setGain(0);
      }
    }
  }
} /* LinkManager::updateDuckingForSink */


float LinkManager::getEffectiveDuckLevel(const std::string& src_name,
                                          const std::string& sink_name)
{
    // Use the DUCK_LEVEL_DB of the link that decided the mode
  const Link* link = modeLink(src_name, sink_name);
  if ((link != nullptr) && (link->audio_mode == LinkAudioMode::DUCK))
  {
    return link->duck_level_db;
  }
  return -12.0f;  // Default
} /* LinkManager::getEffectiveDuckLevel */


bool LinkManager::isSourceFromPriorityLink(const std::string& src_name,
                                            const std::string& sink_name)
{
    // Check if this source is a priority source (PRIORITY_SOURCES) of an
    // activated PRIORITY link that creates the connection to the sink
  return priorityLink(src_name, sink_name) != nullptr;
} /* LinkManager::isSourceFromPriorityLink */


bool LinkManager::isPrioritySourceActive(const std::string& sink_name)
{
    // Check if any PRIORITY source is currently transmitting to this sink
  SinkMap::iterator sink_it = sinks.find(sink_name);
  if (sink_it == sinks.end()) return false;

  for (const auto& amp_pair : sink_it->second.amps)
  {
    const std::string& src_name = amp_pair.first;

      // Only a source with an established connection into this sink counts
      // (e.g. not a muted logic)
    if (current_cons.count(std::make_pair(src_name, sink_name)) == 0) continue;

      // Check if this source's squelch is open
    LogicMap::iterator it = logic_map.find(src_name);
    if (it == logic_map.end() || !it->second.squelch_open) continue;

      // Check if this source is from a PRIORITY link to this sink
    if (isSourceFromPriorityLink(src_name, sink_name))
    {
      return true;
    }
  }
  return false;
} /* LinkManager::isPrioritySourceActive */


float LinkManager::getEffectivePriorityMuteLevel(const std::string& sink_name)
{
    // Use the PRIORITY_MUTE_DB of the PRIORITY link that pre-empts the audio
    // into this sink, either because one of its priority sources is
    // transmitting into the sink or because its hangtime is running for the
    // sink. If more than one link does, the first one in GLOBAL/LINKS wins.
  for (const Link* link : link_order)
  {
    if (!link->is_activated || (link->audio_mode != LinkAudioMode::PRIORITY))
    {
      continue;
    }
    if ((link->priority_hangtime_timer != nullptr) &&
        link->priority_hangtime_timer->isEnabled() &&
        (link->priority_active_sinks.count(sink_name) > 0))
    {
      return link->priority_mute_db;
    }
    for (const auto& con : current_cons)
    {
      if (con.second != sink_name)
      {
        continue;
      }
      LogicMap::const_iterator it = logic_map.find(con.first);
      if ((it != logic_map.end()) && it->second.squelch_open &&
          (priorityLink(con.first, sink_name) == link))
      {
        return link->priority_mute_db;
      }
    }
  }
  return -60.0f;
} /* LinkManager::getEffectivePriorityMuteLevel */


void LinkManager::updatePriorityForSink(const std::string& sink_name)
{
  SinkMap::iterator sink_it = sinks.find(sink_name);
  if (sink_it == sinks.end()) return;

  SinkInfo& sink = sink_it->second;

    // Priority is active if a PRIORITY source is transmitting OR
    // if we're in the hangtime period after a PRIORITY transmission
  bool priority_active = isPrioritySourceActive(sink_name) ||
                         isPriorityHangtimeActive(sink_name);

    // Check if sink logic's squelch is open (for DUCK interaction)
  bool sink_squelch_open = false;
  LogicMap::iterator sink_logic_it = logic_map.find(sink_name);
  if (sink_logic_it != logic_map.end())
  {
    sink_squelch_open = sink_logic_it->second.squelch_open;
  }

    // Handle all connections (both FIRST mode via selector and mixer-based modes)
  for (auto& con_pair : sink.connectors)
  {
    const std::string& src_name = con_pair.first;

      // Only touch connections that are actually established. addLogic wires a
      // connector (and valve/amp) between every pair of logics, but only the
      // connections present in current_cons belong to an activated link. Never
      // enable a selector branch or rewrite an amp gain for a connection that
      // is not currently wanted, otherwise a squelch event would resurrect
      // audio between deactivated or unlinked logics.
    if (current_cons.count(std::make_pair(src_name, sink_name)) == 0)
    {
      continue;
    }

    LinkAudioMode mode = getEffectiveMode(src_name, sink_name);
    bool is_priority_src = isSourceFromPriorityLink(src_name, sink_name);

    if (mode == LinkAudioMode::FIRST)
    {
        // FIRST mode: use selector enable/disable for priority preemption
      if (priority_active && !is_priority_src)
      {
          // Disable this source in the selector when preempted by priority
        sink.selector->disableAutoSelect(con_pair.second);
      }
      else
      {
          // Re-enable in selector (normal operation)
        sink.selector->enableAutoSelect(con_pair.second, 0);
      }
    }
    else
    {
        // MIX/DUCK/PRIORITY mode: use amp gain for priority preemption
      AudioAmp* amp = sink.amps.at(src_name);

        // Determine base gain level (may be ducked if sink squelch is open)
      float base_gain = 0;
      if (sink_squelch_open && mode == LinkAudioMode::DUCK)
      {
        base_gain = getEffectiveDuckLevel(src_name, sink_name);
      }

      if (priority_active && !is_priority_src)
      {
          // A PRIORITY source is active and this is NOT a priority source - mute it
        float mute_level = getEffectivePriorityMuteLevel(sink_name);
        amp->setGain(mute_level);
      }
      else
      {
          // No priority preemption (or this IS the priority source) - use base gain
        amp->setGain(base_gain);
      }
    }
  }
} /* LinkManager::updatePriorityForSink */


bool LinkManager::isPriorityHangtimeActive(const std::string& sink_name)
{
    // Check if any PRIORITY link has an active hangtime timer for this sink.
    // The sink does not have to be a member of the link, since a priority
    // source also reaches the logics that are linked to a member via other
    // links.
  for (const auto& link_pair : links)
  {
    const Link& link = link_pair.second;
    if (!link.is_activated) continue;
    if (link.audio_mode != LinkAudioMode::PRIORITY) continue;

      // Hangtime only mutes the sinks that were actually being preempted by a
      // priority source when it unkeyed. A member whose only activity was its
      // own local transmission is not in priority_active_sinks and so is not
      // muted during hangtime.
    if (link.priority_hangtime_timer != nullptr &&
        link.priority_hangtime_timer->isEnabled() &&
        link.priority_active_sinks.count(sink_name) > 0)
    {
      return true;
    }
  }
  return false;
} /* LinkManager::isPriorityHangtimeActive */


void LinkManager::checkPriorityHangtime(void)
{
    // For each PRIORITY link, check if we need to start or stop the
    // hangtime timer
  for (auto& link_pair : links)
  {
    Link& link = link_pair.second;
    if (!link.is_activated) continue;
    if (link.audio_mode != LinkAudioMode::PRIORITY) continue;
    if (link.priority_hangtime_timer == nullptr) continue;

      // Determine the sinks that a priority source of this link is currently
      // transmitting INTO. Only established connections count, and only
      // the ones for which this link makes the source a priority source
      // (see priorityLink), so a member that is not a priority source
      // (PRIORITY_SOURCES) never arms the hangtime. A member's own open
      // squelch never counts as priority activity for itself (a logic is
      // never connected to itself); it only counts as a priority source into
      // the OTHER logics. Ordinary local traffic on a member therefore never
      // arms hangtime muting of that member's own incoming audio.
    std::set<std::string> active_sinks;
    for (const auto& con : current_cons)
    {
      const std::string& src_name = con.first;
      const std::string& sink_name = con.second;
      LogicMap::iterator it = logic_map.find(src_name);
      if ((it != logic_map.end()) && it->second.squelch_open &&
          (priorityLink(src_name, sink_name) == &link))
      {
        active_sinks.insert(sink_name);
      }
    }

    if (!active_sinks.empty())
    {
        // A priority source is transmitting into these sinks - stop any
        // running hangtime timer and remember which sinks are being preempted
        // so the hangtime (once the source unkeys) applies to exactly them.
      link.priority_hangtime_timer->setEnable(false);
      link.priority_active_sinks = active_sinks;
    }
    else if (!link.priority_active_sinks.empty())
    {
        // Priority was active but is now inactive.
        // Start hangtime timer if it's not already running.
      if (!link.priority_hangtime_timer->isEnabled())
      {
        link.priority_hangtime_timer->reset();
        link.priority_hangtime_timer->setEnable(true);
      }
    }
  }
} /* LinkManager::checkPriorityHangtime */


void LinkManager::onPriorityHangtimeExpired(Async::Timer *t, Link *link)
{
  t->setEnable(false);

    // Hangtime expired - refresh the sinks that were being held muted, then
    // forget them.
  std::set<std::string> affected;
  affected.swap(link->priority_active_sinks);
  for (const auto& sink_name : affected)
  {
    updatePriorityForSink(sink_name);
  }
} /* LinkManager::onPriorityHangtimeExpired */


/*
 * This file has not been truncated
 */
