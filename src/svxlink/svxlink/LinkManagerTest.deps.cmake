# LinkManagerTest needs extra svxlink core sources + svxmisc (for setValueFromString
# etc used in LinkManager::initialize and common) and asyncaudio (for FakeLogic).
#
# LinkManager.cpp does a dynamic_cast<Logic*>, so Logic.cpp (and its transitive
# helpers EventHandler/MsgHandler/Module/QsoRecorder/...) must be linked in,
# which in turn pulls in the trx, locationinfo and TCL libraries that those
# sources reference. These library variables are defined in the enclosing
# CMakeLists.txt (which builds the svxlink binary from the same set).
set(LinkManagerTest_EXTRA_SRCS
  MsgHandler.cpp Module.cpp Logic.cpp EventHandler.cpp
  LinkManager.cpp CmdParser.cpp QsoRecorder.cpp DtmfDigitHandler.cpp
)
set(LinkManagerTest_EXTRA_LIBS
  trx
  locationinfo
  asyncaudio
  svxmisc
  ${TCL_LIBRARY}
  ${JSONCPP_LIBRARIES}
  ${CURL_LIBRARIES}
  ${CURL_LIBRARY}
  ${GSM_LIBRARY}
  ${POPT_LIBRARIES}
  ${DL_LIBRARIES}
)
