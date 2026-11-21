#pragma once

#define IDI_APP 101

#define IDD_MAIN           200
#define IDD_SYSTEM_MESSAGE 201
#define IDD_TOOLS          202
#define IDD_MCP_SERVERS    203
#define IDD_MCP_EDIT       204
#define IDD_SETTINGS       205
#define IDD_RENAME         206
#define IDD_AUTO           207
#define IDD_PROVIDERS      208
#define IDD_PROVIDER_EDIT  209

// main window
#define IDC_NEW_CHAT          1001
#define IDC_SEARCH            1002
#define IDC_MCP_SERVERS       1003
#define IDC_SETTINGS          1004
#define IDC_RECENT_LABEL      1005
#define IDC_CONVERSATIONS     1006
#define IDC_WORKSPACE         1007
#define IDC_WORKSPACE_CHANGE  1008
#define IDC_TRANSCRIPT        1009
#define IDC_HELLO             1010
#define IDC_HELLO_SUB         1011
#define IDC_ATTACHMENTS       1012
#define IDC_CLEAR_ATTACHMENTS 1013
#define IDC_INPUT             1014
#define IDC_PLUS              1015
#define IDC_MODEL             1016
#define IDC_REFRESH_MODELS    1017
#define IDC_SEND              1018
#define IDC_STATUS            1019
#define IDC_TITLE             1020
#define IDC_PROVIDERS         1021
#define IDC_MIC               1022

// system message
#define IDC_SYSTEM_TEXT    1101
#define IDC_SYSTEM_DEFAULT 1102

// tools
#define IDC_TOOLS_TREE 1201
#define IDC_TOOLS_ALL  1202
#define IDC_TOOLS_NONE 1203

// MCP servers
#define IDC_MCP_LIST          1301
#define IDC_MCP_ADD           1302
#define IDC_MCP_EDIT          1303
#define IDC_MCP_REMOVE        1304
#define IDC_MCP_RECONNECT     1305
#define IDC_MCP_NAME          1311
#define IDC_MCP_TRANSPORT     1312
#define IDC_MCP_COMMAND       1313
#define IDC_MCP_URL           1314
#define IDC_MCP_TOKEN         1315
#define IDC_MCP_ENABLED       1316
#define IDC_MCP_COMMAND_LABEL 1317
#define IDC_MCP_URL_LABEL     1318
#define IDC_MCP_TOKEN_LABEL   1319

// settings
#define IDC_BACKEND_HUB       1401
#define IDC_BACKEND_CUSTOM    1402
#define IDC_CUSTOM_URL        1403
#define IDC_CUSTOM_KEY        1404
#define IDC_DEFAULT_WORKSPACE 1405
#define IDC_BROWSE_WORKSPACE  1406
#define IDC_SHELL             1407
#define IDC_CONFIRM_SHELL     1408
#define IDC_CONFIRM_WRITES    1409
#define IDC_SHOW_REASONING    1410
#define IDC_MAX_ROUNDS        1411
#define IDC_TEMPERATURE       1412
#define IDC_OPEN_MANAGER      1413
#define IDC_DATA_FOLDER       1414
#define IDC_AUTO_START_HUB    1415

// rename
#define IDC_RENAME_TEXT 1501

// Auto mode
#define IDC_AUTO_CAT_FIRST 1601  // + category index (6 combo boxes)
#define IDC_AUTO_ROUTER    1610
#define IDC_AUTO_STICKY    1611
#define IDC_AUTO_SUGGEST   1612

// model providers
#define IDC_PROV_LIST       1701
#define IDC_PROV_ADD        1702
#define IDC_PROV_EDIT       1703
#define IDC_PROV_REMOVE     1704
#define IDC_PROV_DETECT     1705
#define IDC_PROV_NOTE       1706
#define IDC_PROV_AUTOIMPORT 1707
#define IDC_PE_PRESET       1711
#define IDC_PE_NAME         1712
#define IDC_PE_KIND         1713
#define IDC_PE_URL          1714
#define IDC_PE_KEY          1715
#define IDC_PE_KEYENV       1716
#define IDC_PE_ENABLED      1717
#define IDC_PE_AUTO         1718
#define IDC_PE_TEST         1719
#define IDC_PE_RESULT       1720
#define IDC_PE_KEYPAGE      1721
#define IDC_PE_PRIVACY      1722
#define IDC_PE_SIGNIN       1723

// "+" menu and conversation menu (TrackPopupMenu with TPM_RETURNCMD)
#define ID_REASONING_FIRST 40001  // + 0..4: default, off, low, medium, high
#define ID_ADD_FILES       40010
#define ID_SYSTEM_MESSAGE  40011
#define ID_TOOLS           40012
#define ID_AUTO_SETTINGS   40015
#define ID_PROVIDERS       40016
#define ID_PASTE           40017
#define ID_OLLAMA_MOVE     40018
#define ID_CLAUDE_SETUP    40019
#define ID_MODEL_REFRESH   40020
#define ID_EXPORT          40021
#define ID_NEW_CHAT_MENU   40022
#define ID_MCP_MENU        40023
#define ID_SETTINGS_MENU   40024
#define ID_WORKSPACE_MENU  40025
#define ID_MODEL_FIRST     41000  // + index in comboIds_

// Claude setup
#define IDD_CLAUDE            210
#define IDC_CL_INTRO          1801
#define IDC_CL_STATE          1802
#define IDC_CL_INSTALL        1803
#define IDC_CL_SIGNIN         1804
#define IDC_CL_KEYPAGE        1805
#define ID_MCP_MANAGE         40013
#define ID_MCP_RECONNECT      40014
#define ID_MCP_TOGGLE_FIRST   40100   // + server index
#define ID_MCP_RESOURCE_FIRST 40300  // + resource index
#define ID_CONV_RENAME        40500
#define ID_CONV_DELETE        40501
#define ID_CONV_OPEN_FOLDER   40502
#define ID_CONV_EXPORT        40503
