#include "include/opl.h"
#include "include/lang.h"
#include "include/gui.h"
#include "include/supportbase.h"
#include "include/ethsupport.h"
#include "include/util.h"
#include "include/renderman.h"
#include "include/themes.h"
#include "include/textures.h"
#include "include/ioman.h"
#include "include/system.h"
#include "include/extern_irx.h"
#ifdef CHEAT
#include "include/cheatman.h"
#endif
#include "modules/iopcore/common/cdvd_config.h"

#include "include/nbns.h"
#include "httpclient.h"

static char ethPrefix[40]; //Contains the full path to the folder where all the games are.
static char *ethBase;
static int ethULSizePrev = -2;
static unsigned char ethModifiedCDPrev[8];
static unsigned char ethModifiedDVDPrev[8];
static int ethGameCount = 0;
static unsigned char ethModulesLoaded = 0;
static base_game_info_t *ethGames = NULL;

// forward declaration
static item_list_t ethGameList;
static int ethGetNetIFLinkStatus(void);

static int ethInitSemaID = -1;

//Initializes locking semaphore for network support (not for just SMB support, but for the network subsystem).
static int ethInitSema(void)
{
    if (ethInitSemaID < 0) {
        if ((ethInitSemaID = sbCreateSemaphore()) < 0)
            return ethInitSemaID;
    }

    return 0;
}

static void ethSMBConnect(void)
{
    unsigned char share_ip_address[4];
    smbLogOn_in_t logon;
    smbEcho_in_t echo;
    smbOpenShare_in_t openshare;
    int result;

    if (gETHPrefix[0] != '\0')
        sprintf(ethPrefix, "%s%s\\", ethBase, gETHPrefix);
    else
        strcpy(ethPrefix, ethBase);

    // open tcp connection with the server / logon to SMB server
    if (gPCShareAddressIsNetBIOS) {
        if (nbnsFindName(gPCShareNBAddress, share_ip_address) != 0) {
            gNetworkStartup = ERROR_ETH_SMB_CONN;
            return;
        }

        sprintf(logon.serverIP, "%u.%u.%u.%u", share_ip_address[0], share_ip_address[1], share_ip_address[2], share_ip_address[3]);
    } else {
        sprintf(logon.serverIP, "%u.%u.%u.%u", pc_ip[0], pc_ip[1], pc_ip[2], pc_ip[3]);
    }

    logon.serverPort = gPCPort;

    if (strlen(gPCPassword) > 0) {
        smbGetPasswordHashes_in_t passwd;
        smbGetPasswordHashes_out_t passwdhashes;

        // we'll try to generate hashed password first
        strncpy(logon.User, gPCUserName, sizeof(logon.User));
        strncpy(passwd.password, gPCPassword, sizeof(passwd.password));

        if (fileXioDevctl(ethBase, SMB_DEVCTL_GETPASSWORDHASHES, (void *)&passwd, sizeof(passwd), (void *)&passwdhashes, sizeof(passwdhashes)) == 0) {
            // hash generated okay, can use
            memcpy((void *)logon.Password, (void *)&passwdhashes, sizeof(passwdhashes));
            logon.PasswordType = HASHED_PASSWORD;
            memcpy((void *)openshare.Password, (void *)&passwdhashes, sizeof(passwdhashes));
            openshare.PasswordType = HASHED_PASSWORD;
        } else {
            // failed hashing, failback to plaintext
            strncpy(logon.Password, gPCPassword, sizeof(logon.Password));
            logon.PasswordType = PLAINTEXT_PASSWORD;
            strncpy(openshare.Password, gPCPassword, sizeof(openshare.Password));
            openshare.PasswordType = PLAINTEXT_PASSWORD;
        }
    } else {
        strncpy(logon.User, gPCUserName, sizeof(logon.User));
        logon.PasswordType = NO_PASSWORD;
        openshare.PasswordType = NO_PASSWORD;
    }

    if ((result = fileXioDevctl(ethBase, SMB_DEVCTL_LOGON, (void *)&logon, sizeof(logon), NULL, 0)) >= 0) {
        // SMB server alive test
        strcpy(echo.echo, "ALIVE ECHO TEST");
        echo.len = strlen("ALIVE ECHO TEST");

        if (gPCShareAddressIsNetBIOS) {
            // Since the SMB server can be connected to, update the IP address.
            pc_ip[0] = share_ip_address[0];
            pc_ip[1] = share_ip_address[1];
            pc_ip[2] = share_ip_address[2];
            pc_ip[3] = share_ip_address[3];
        }

        if (fileXioDevctl(ethBase, SMB_DEVCTL_ECHO, (void *)&echo, sizeof(echo), NULL, 0) >= 0) {
            gNetworkStartup = ERROR_ETH_SMB_OPENSHARE;

            if (gPCShareName[0]) {
                // connect to the share
                strcpy(openshare.ShareName, gPCShareName);

                if (fileXioDevctl(ethBase, SMB_DEVCTL_OPENSHARE, (void *)&openshare, sizeof(openshare), NULL, 0) >= 0) {
                    // everything is ok
                    gNetworkStartup = 0;
                }
            }
        } else {
            gNetworkStartup = ERROR_ETH_SMB_ECHO;
        }
    } else {
        gNetworkStartup = (result == -SMB_DEVCTL_LOGON_ERR_CONN) ? ERROR_ETH_SMB_CONN : ERROR_ETH_SMB_LOGON;
    }
}

static int ethSMBDisconnect(void)
{
    int ret;

    // closing share
    ret = fileXioDevctl(ethBase, SMB_DEVCTL_CLOSESHARE, NULL, 0, NULL, 0);
    if (ret < 0)
        return -1;

    // logoff/close tcp connection from SMB server:
    ret = fileXioDevctl(ethBase, SMB_DEVCTL_LOGOFF, NULL, 0, NULL, 0);
    if (ret < 0)
        return -2;

    return 0;
}

static void EthStatusCheckCb(s32 alarm_id, u16 time, void *common)
{
    iSignalSema(*(int *)common);
}

static int WaitValidNetState(int (*checkingFunction)(void))
{
    int SemaID, retry_cycles;
    ee_sema_t SemaData;

    // Wait for a valid network status;
    SemaData.option = SemaData.attr = 0;
    SemaData.init_count = 0;
    SemaData.max_count = 1;
    if ((SemaID = CreateSema(&SemaData)) < 0)
        return SemaID;

    for (retry_cycles = 0; checkingFunction() == 0; retry_cycles++) {
        SetAlarm(1000 * rmGetHsync(), &EthStatusCheckCb, &SemaID);
        WaitSema(SemaID);

        if (retry_cycles >= 30) //30s = 30*1000ms
        {
            DeleteSema(SemaID);
            return -1;
        }
    }

    DeleteSema(SemaID);
    return 0;
}

int ethWaitValidNetIFLinkState(void)
{
    return WaitValidNetState(&ethGetNetIFLinkStatus);
}

int ethWaitValidDHCPState(void)
{
    return WaitValidNetState(&ethGetDHCPStatus);
}

static int ethInitApplyConfig(void)
{
    LOG("ETHSUPPORT ApplyConfig\n");

    do {
        if (ethWaitValidNetIFLinkState() != 0) {
            gNetworkStartup = ERROR_ETH_LINK_FAIL;
            return ERROR_ETH_LINK_FAIL;
        }
    } while (ethApplyNetIFConfig() != 0);

    //Before the network configuration is applied, wait for a valid link status.
    if (ethWaitValidNetIFLinkState() != 0) {
        gNetworkStartup = ERROR_ETH_LINK_FAIL;
        return ERROR_ETH_LINK_FAIL;
    }

    ethApplyIPConfig();

    //Wait for DHCP to initialize, if DHCP is enabled.
    if (ps2_ip_use_dhcp && (ethWaitValidDHCPState() != 0)) {
        gNetworkStartup = ERROR_ETH_DHCP_FAIL;
        return ERROR_ETH_DHCP_FAIL;
    }

    return 0;
}

static void ethInitSMB(void)
{
    int ret;

    WaitSema(ethInitSemaID);
    ret = ethInitApplyConfig();
    SignalSema(ethInitSemaID);

    if (ret != 0) {
        ethDisplayErrorStatus();
        return;
    }

    // connect
    ethSMBConnect();

    if (gNetworkStartup == 0) {
        // update Themes
        char path[256];
        sprintf(path, "%sTHM", ethPrefix);
        thmAddElements(path, "\\", ethGameList.mode);

        sbCreateFolders(ethPrefix, 1);
    } else if (gPCShareName[0] || !(gNetworkStartup >= ERROR_ETH_SMB_OPENSHARE)) {
        ethDisplayErrorStatus();
    }
}

static int ethLoadModules(void)
{
    LOG("ETHSUPPORT LoadModules\n");

    if (!ethModulesLoaded) {
        ethModulesLoaded = 1;

        sysInitDev9();

        if (sysLoadModuleBuffer(&netman_irx, size_netman_irx, 0, NULL) >= 0) {
            NetManInit();

            sysLoadModuleBuffer(&smsutils_irx, size_smsutils_irx, 0, NULL);
            if (sysLoadModuleBuffer(&smap_irx, size_smap_irx, 0, NULL) >= 0) {
                //Before the network stack is loaded, attempt to set the link settings in order to avoid needing double-initialization of the IF.
                //But do not fail here because there is currently no way to re-start initialization.
                ethApplyNetIFConfig();

                if (sysLoadModuleBuffer(&ps2ip_irx, size_ps2ip_irx, 0, NULL) >= 0) {
                    sysLoadModuleBuffer(&ps2ips_irx, size_ps2ips_irx, 0, NULL);
                    sysLoadModuleBuffer(&httpclient_irx, size_httpclient_irx, 0, NULL);
                    ps2ip_init();
                    HttpInit();

                    LOG("ETHSUPPORT Modules loaded\n");
                    return 0;
                }
            }
        }

        gNetworkStartup = ERROR_ETH_MODULE_NETIF_FAILURE;
        return -1;
    }

    return 0;
}

void ethDeinitModules(void)
{
    if (ethInitSemaID >= 0)
        WaitSema(ethInitSemaID);

    HttpDeinit();
    nbnsDeinit();
    NetManDeinit();
    ethModulesLoaded = 0;
    gNetworkStartup = ERROR_ETH_NOT_STARTED;

    if (ethInitSemaID >= 0) {
        DeleteSema(ethInitSemaID);
        ethInitSemaID = -1;
    }
}

int ethLoadInitModules(void)
{
    int ret;

    if ((ret = ethInitSema()) < 0)
        return ret;

    WaitSema(ethInitSemaID);

    if ((ret = ethLoadModules()) == 0) {
        ret = ethInitApplyConfig();
    }

    SignalSema(ethInitSemaID);

    return ret;
}

void ethDisplayErrorStatus(void)
{
    switch (gNetworkStartup) {
        case 0: //No error
            break;
        case ERROR_ETH_MODULE_NETIF_FAILURE:
            setErrorMessageWithCode(_STR_NETWORK_STARTUP_ERROR_NETIF, gNetworkStartup);
            break;
        case ERROR_ETH_SMB_CONN:
            setErrorMessageWithCode(_STR_NETWORK_STARTUP_ERROR_CONN, gNetworkStartup);
            break;
        case ERROR_ETH_SMB_LOGON:
            setErrorMessageWithCode(_STR_NETWORK_STARTUP_ERROR_LOGON, gNetworkStartup);
            break;
        case ERROR_ETH_SMB_OPENSHARE:
            setErrorMessageWithCode(_STR_NETWORK_STARTUP_ERROR_SHARE, gNetworkStartup);
            break;
        case ERROR_ETH_SMB_LISTSHARES:
            setErrorMessageWithCode(_STR_NETWORK_SHARE_LIST_ERROR, gNetworkStartup);
            break;
        case ERROR_ETH_SMB_LISTGAMES:
            setErrorMessageWithCode(_STR_NETWORK_GAMES_LIST_ERROR, gNetworkStartup);
            break;
        case ERROR_ETH_LINK_FAIL:
            LOG("ETH: Unable to get valid link status.\n");
            setErrorMessageWithCode(_STR_NETWORK_ERROR_LINK_FAIL, gNetworkStartup);
            break;
        case ERROR_ETH_DHCP_FAIL:
            LOG("ETH: Unable to get valid IP address via DHCP.\n");
            setErrorMessageWithCode(_STR_NETWORK_ERROR_DHCP_FAIL, gNetworkStartup);
            break;
        default:
            setErrorMessageWithCode(_STR_NETWORK_STARTUP_ERROR, gNetworkStartup);
    }
}

static void smbLoadModules(void)
{
    int ret;

    LOG("SMBSUPPORT LoadModules\n");

    WaitSema(ethInitSemaID);
    ret = ethLoadModules();
    SignalSema(ethInitSemaID);

    if (ret == 0) {
        gNetworkStartup = ERROR_ETH_MODULE_SMBMAN_FAILURE;
        if (sysLoadModuleBuffer(&smbman_irx, size_smbman_irx, 0, NULL) >= 0) {
            sysLoadModuleBuffer(&nbns_irx, size_nbns_irx, 0, NULL);
            nbnsInit();

            LOG("SMBSUPPORT Modules loaded\n");
            ethInitSMB();
            return;
        }
    }

    ethDisplayErrorStatus();
}

void ethInit(void)
{
    if (ethInitSema() < 0)
        return;

    if (gNetworkStartup >= ERROR_ETH_SMB_CONN) {
        LOG("ETHSUPPORT Re-Init\n");
        thmReinit(ethBase);
        ethULSizePrev = -2;
        ethGameCount = 0;
        ioPutRequest(IO_CUSTOM_SIMPLEACTION, &ethInitSMB);
    } else {
        LOG("ETHSUPPORT Init\n");
        ethBase = "smb0:";
        ethULSizePrev = -2;
        memset(ethModifiedCDPrev, 0, sizeof(ethModifiedCDPrev));
        memset(ethModifiedDVDPrev, 0, sizeof(ethModifiedDVDPrev));
        ethGameCount = 0;
        ethGames = NULL;
        configGetInt(configGetByType(CONFIG_OPL), "eth_frames_delay", &ethGameList.delay);
        gNetworkStartup = ERROR_ETH_NOT_STARTED;
        ioPutRequest(IO_CUSTOM_SIMPLEACTION, &smbLoadModules);
        ethGameList.enabled = 1;
    }
}

item_list_t *ethGetObject(int initOnly)
{
    if (initOnly && !ethGameList.enabled)
        return NULL;
    return &ethGameList;
}

static int ethNeedsUpdate(void)
{
    int result;

    result = 0;

    if (ethULSizePrev == -2)
        result = 1;

    if (gNetworkStartup == 0) {
        iox_stat_t stat;
        char path[256];

        sprintf(path, "%sCD", ethPrefix);
        if (fileXioGetStat(path, &stat) != 0)
            memset(stat.mtime, 0, sizeof(stat.mtime));
        if (memcmp(ethModifiedCDPrev, stat.mtime, sizeof(ethModifiedCDPrev))) {
            memcpy(ethModifiedCDPrev, stat.mtime, sizeof(ethModifiedCDPrev));
            result = 1;
        }

        sprintf(path, "%sDVD", ethPrefix);
        if (fileXioGetStat(path, &stat) != 0)
            memset(stat.mtime, 0, sizeof(stat.mtime));
        if (memcmp(ethModifiedDVDPrev, stat.mtime, sizeof(ethModifiedDVDPrev))) {
            memcpy(ethModifiedDVDPrev, stat.mtime, sizeof(ethModifiedDVDPrev));
            result = 1;
        }

        if (!sbIsSameSize(ethPrefix, ethULSizePrev))
            result = 1;
    }

    return result;
}

static int ethUpdateGameList(void)
{
    int result;

    if (gPCShareName[0]) {
        if (gNetworkStartup != 0)
            return 0;

        if ((result = sbReadList(&ethGames, ethPrefix, &ethULSizePrev, &ethGameCount)) < 0) {
            gNetworkStartup = ERROR_ETH_SMB_LISTGAMES;
            ethDisplayErrorStatus();
        }
    } else {
        int i, count;
        ShareEntry_t sharelist[128];
        smbGetShareList_in_t getsharelist;

        if (gNetworkStartup < ERROR_ETH_SMB_OPENSHARE)
            return 0;

        getsharelist.EE_addr = (void *)&sharelist[0];
        getsharelist.maxent = 128;

        count = fileXioDevctl(ethBase, SMB_DEVCTL_GETSHARELIST, (void *)&getsharelist, sizeof(getsharelist), NULL, 0);
        if (count > 0) {
            free(ethGames);
            ethGames = (base_game_info_t *)malloc(sizeof(base_game_info_t) * count);
            for (i = 0; i < count; i++) {
                LOG("ETHSUPPORT Share found: %s\n", sharelist[i].ShareName);
                base_game_info_t *g = &ethGames[i];
                memcpy(g->name, sharelist[i].ShareName, sizeof(g->name));
                g->name[31] = '\0';
                sprintf(g->startup, "SHARE");
                g->extension[0] = '\0';
                g->parts = 0x00;
                g->media = 0x00;
                g->format = GAME_FORMAT_USBLD;
                g->sizeMB = 0;
            }
            ethGameCount = count;
        } else if (count < 0) {
            gNetworkStartup = ERROR_ETH_SMB_LISTSHARES;
            ethDisplayErrorStatus();
        }
    }
    return ethGameCount;
}

static int ethGetGameCount(void)
{
    return ethGameCount;
}

static void *ethGetGame(int id)
{
    return (void *)&ethGames[id];
}

static char *ethGetGameName(int id)
{
    return ethGames[id].name;
}

static int ethGetGameNameLength(int id)
{
    if (ethGames[id].format != GAME_FORMAT_USBLD)
        return ISO_GAME_NAME_MAX + 1;
    else
        return UL_GAME_NAME_MAX + 1;
}

static char *ethGetGameStartup(int id)
{
    return ethGames[id].startup;
}

#ifndef __CHILDPROOF
static void ethDeleteGame(int id)
{
    sbDelete(&ethGames, ethPrefix, "\\", ethGameCount, id);
    ethULSizePrev = -2;
}

static void ethRenameGame(int id, char *newName)
{
    sbRename(&ethGames, ethPrefix, "\\", ethGameCount, id, newName);
    ethULSizePrev = -2;
}
#endif

static void ethLaunchGame(int id, config_set_t *configSet)
{
    int i, compatmask;
    int EnablePS2Logo = 0;
#ifdef CHEAT
    int result;
#endif
    char filename[32], partname[256];
    base_game_info_t *game = &ethGames[id];
    struct cdvdman_settings_smb *settings;
    u32 layer1_start, layer1_offset;
    unsigned short int layer1_part;

    if (!gPCShareName[0]) {
        memcpy(gPCShareName, game->name, sizeof(gPCShareName));
        ethULSizePrev = -2;
        ethGameCount = 0;
        ioPutRequest(IO_MENU_UPDATE_DEFFERED, &ethGameList.mode); // clear the share list
        ioPutRequest(IO_CUSTOM_SIMPLEACTION, &ethInitSMB);
        ioPutRequest(IO_MENU_UPDATE_DEFFERED, &ethGameList.mode); // reload the game list
        return;
    }

#ifdef VMC
    char vmc_name[32];
    int vmc_id, size_mcemu_irx = 0;
    smb_vmc_infos_t smb_vmc_infos;
    vmc_superblock_t vmc_superblock;

    for (vmc_id = 0; vmc_id < 2; vmc_id++) {
        memset(&smb_vmc_infos, 0, sizeof(smb_vmc_infos_t));
        configGetVMC(configSet, vmc_name, sizeof(vmc_name), vmc_id);
        if (vmc_name[0]) {
            if (sysCheckVMC(ethPrefix, "\\", vmc_name, 0, &vmc_superblock) > 0) {
                smb_vmc_infos.flags = vmc_superblock.mc_flag & 0xFF;
                smb_vmc_infos.flags |= 0x100;
                smb_vmc_infos.specs.page_size = vmc_superblock.page_size;
                smb_vmc_infos.specs.block_size = vmc_superblock.pages_per_block;
                smb_vmc_infos.specs.card_size = vmc_superblock.pages_per_cluster * vmc_superblock.clusters_per_card;
                smb_vmc_infos.active = 1;
                smb_vmc_infos.fid = 0xFFFF;
                if (gETHPrefix[0])
                    snprintf(smb_vmc_infos.fname, sizeof(smb_vmc_infos.fname), "%s\\VMC\\%s.bin", gETHPrefix, vmc_name);
                else
                    snprintf(smb_vmc_infos.fname, sizeof(smb_vmc_infos.fname), "VMC\\%s.bin", vmc_name);
            } else {
                char error[256];
                snprintf(error, sizeof(error), _l(_STR_ERR_VMC_CONTINUE), vmc_name, (vmc_id + 1));
                if (!guiMsgBox(error, 1, NULL))
                    return;
            }
        }

        for (i = 0; i < size_smb_mcemu_irx; i++) {
            if (((u32 *)&smb_mcemu_irx)[i] == (0xC0DEFAC0 + vmc_id)) {
                if (smb_vmc_infos.active)
                    size_mcemu_irx = size_smb_mcemu_irx;
                memcpy(&((u32 *)&smb_mcemu_irx)[i], &smb_vmc_infos, sizeof(smb_vmc_infos_t));
                break;
            }
        }
    }
#endif

    if (gRememberLastPlayed) {
        configSetStr(configGetByType(CONFIG_LAST), "last_played", game->startup);
        saveConfig(CONFIG_LAST, 0);
    }

    compatmask = sbPrepare(game, configSet, size_smb_cdvdman_irx, &smb_cdvdman_irx, &i);

#ifdef CHEAT
    if ((result = sbLoadCheats(ethPrefix, game->startup)) < 0) {
        switch (result) {
            case -ENOENT:
                guiWarning(_l(_STR_NO_CHEATS_FOUND), 10);
                break;
            default:
                guiWarning(_l(_STR_ERR_CHEATS_LOAD_FAILED), 10);
        }
    }
#endif

    settings = (struct cdvdman_settings_smb *)((u8 *)(&smb_cdvdman_irx) + i);

    switch (game->format) {
        case GAME_FORMAT_OLD_ISO:
            sprintf(settings->filename, "%s.%s%s", game->startup, game->name, game->extension);
            break;
        case GAME_FORMAT_ISO:
            sprintf(settings->filename, "%s%s", game->name, game->extension);
            break;
        default: //USBExtreme format.
            sprintf(settings->filename, "ul.%08X.%s", USBA_crc32(game->name), game->startup);
            settings->common.flags |= IOPCORE_SMB_FORMAT_USBLD;
    }

    sprintf(settings->smb_ip, "%u.%u.%u.%u", pc_ip[0], pc_ip[1], pc_ip[2], pc_ip[3]);
    settings->smb_port = gPCPort;
    strcpy(settings->smb_share, gPCShareName);
    strcpy(settings->smb_prefix, gETHPrefix);
    strcpy(settings->smb_user, gPCUserName);
    strcpy(settings->smb_password, gPCPassword);

    //Initialize layer 1 information.
    switch (game->format) {
        case GAME_FORMAT_USBLD:
            sprintf(partname, "%s%s.00", ethPrefix, settings->filename);
            break;
        default: //Raw ISO9660 disc image; one part.
            sprintf(partname, "%s%s\\%s", ethPrefix, game->media == 0x12 ? "CD" : "DVD", settings->filename);
    }

    if (gPS2Logo) {
        int fd = fileXioOpen(partname, O_RDONLY, 0666);
        if (fd >= 0) {
            EnablePS2Logo = CheckPS2Logo(fd, 0);
            fileXioClose(fd);
        }
    }

    layer1_start = sbGetISO9660MaxLBA(partname);

    switch (game->format) {
        case GAME_FORMAT_USBLD:
            layer1_part = layer1_start / 0x80000;
            layer1_offset = layer1_start % 0x80000;
            sprintf(partname, "%s%s.%02x", ethPrefix, settings->filename, layer1_part);
            break;
        default: //Raw ISO9660 disc image; one part.
            layer1_part = 0;
            layer1_offset = layer1_start;
    }

    if (sbProbeISO9660_64(partname, game, layer1_offset) != 0) {
        layer1_start = 0;
        LOG("DVD detected.\n");
    } else {
        layer1_start -= 16;
        LOG("DVD-DL layer 1 @ part %u sector 0x%lx.\n", layer1_part, layer1_offset);
    }
    settings->common.layer1_start = layer1_start;

    // disconnect from the active SMB session
    ethSMBDisconnect();

    if (configGetStrCopy(configSet, CONFIG_ITEM_ALTSTARTUP, filename, sizeof(filename)) == 0)
        strcpy(filename, game->startup);
    deinit(NO_EXCEPTION); // CAREFUL: deinit will call ethCleanUp, so ethGames/game will be freed

#ifdef VMC
#define ETH_MCEMU size_mcemu_irx, &smb_mcemu_irx,
#else
#define ETH_MCEMU 0, NULL,
#endif

    sysLaunchLoaderElf(filename,  "ETH_MODE", size_smb_cdvdman_irx, &smb_cdvdman_irx, ETH_MCEMU EnablePS2Logo, compatmask);
}

static config_set_t *ethGetConfig(int id)
{
    return sbPopulateConfig(&ethGames[id], ethPrefix, "\\");
}

static int ethGetImage(char *folder, int isRelative, char *value, char *suffix, GSTEXTURE *resultTex, short psm)
{
    char path[256];
    if (isRelative)
        snprintf(path, sizeof(path), "%s%s\\%s_%s", ethPrefix, folder, value, suffix);
    else
        snprintf(path, sizeof(path), "%s%s_%s", folder, value, suffix);
    return texDiscoverLoad(resultTex, path, -1, psm);
}

static void ethCleanUp(int exception)
{
    if (ethGameList.enabled) {
        LOG("ETHSUPPORT CleanUp\n");

        free(ethGames);
    }

    ethDeinitModules();
}

#ifdef VMC
static int ethCheckVMC(char *name, int createSize)
{
    return sysCheckVMC(ethPrefix, "\\", name, createSize, NULL);
}
#endif

static item_list_t ethGameList = {
    ETH_MODE, 0, 0, MENU_MIN_INACTIVE_FRAMES, ETH_MODE_UPDATE_DELAY, "ETH Games", _STR_NET_GAMES, &ethInit, &ethNeedsUpdate,
#ifdef __CHILDPROOF
    &ethUpdateGameList, &ethGetGameCount, &ethGetGame, &ethGetGameName, &ethGetGameNameLength, &ethGetGameStartup, NULL, NULL,
#else
    &ethUpdateGameList, &ethGetGameCount, &ethGetGame, &ethGetGameName, &ethGetGameNameLength, &ethGetGameStartup, &ethDeleteGame, &ethRenameGame,
#endif
#ifdef VMC
    &ethLaunchGame, &ethGetConfig, &ethGetImage, &ethCleanUp, &ethCheckVMC, ETH_ICON
#else
    &ethLaunchGame, &ethGetConfig, &ethGetImage, &ethCleanUp, ETH_ICON
#endif
};

int ethGetNetConfig(u8 *ip_address, u8 *netmask, u8 *gateway)
{
    t_ip_info ip_info;
    int result;

    if ((result = ps2ip_getconfig("sm0", &ip_info)) >= 0) {
        ip_address[0] = ip4_addr1((struct ip4_addr *)&ip_info.ipaddr);
        ip_address[1] = ip4_addr2((struct ip4_addr *)&ip_info.ipaddr);
        ip_address[2] = ip4_addr3((struct ip4_addr *)&ip_info.ipaddr);
        ip_address[3] = ip4_addr4((struct ip4_addr *)&ip_info.ipaddr);

        netmask[0] = ip4_addr1((struct ip4_addr *)&ip_info.netmask);
        netmask[1] = ip4_addr2((struct ip4_addr *)&ip_info.netmask);
        netmask[2] = ip4_addr3((struct ip4_addr *)&ip_info.netmask);
        netmask[3] = ip4_addr4((struct ip4_addr *)&ip_info.netmask);

        gateway[0] = ip4_addr1((struct ip4_addr *)&ip_info.gw);
        gateway[1] = ip4_addr2((struct ip4_addr *)&ip_info.gw);
        gateway[2] = ip4_addr3((struct ip4_addr *)&ip_info.gw);
        gateway[3] = ip4_addr4((struct ip4_addr *)&ip_info.gw);
    } else {
        memset(ip_address, 0, sizeof(ip_address));
        memset(netmask, 0, sizeof(netmask));
        memset(gateway, 0, sizeof(gateway));
    }

    return result;
}

int ethApplyNetIFConfig(void)
{
    int mode, result;
    static int CurrentMode = NETMAN_NETIF_ETH_LINK_MODE_AUTO;

    switch (gETHOpMode) {
        case ETH_OP_MODE_100M_FDX:
            mode = NETMAN_NETIF_ETH_LINK_MODE_100M_FDX;
            break;
        case ETH_OP_MODE_100M_HDX:
            mode = NETMAN_NETIF_ETH_LINK_MODE_100M_HDX;
            break;
        case ETH_OP_MODE_10M_FDX:
            mode = NETMAN_NETIF_ETH_LINK_MODE_10M_FDX;
            break;
        case ETH_OP_MODE_10M_HDX:
            mode = NETMAN_NETIF_ETH_LINK_MODE_10M_HDX;
            break;
        default:
            mode = NETMAN_NETIF_ETH_LINK_MODE_AUTO;
    }

    if (CurrentMode != mode) {
        if ((result = NetManSetLinkMode(mode)) == 0)
            CurrentMode = mode;
    } else
        result = 0;

    return result;
}

static int ethGetNetIFLinkStatus(void)
{
    return (NetManIoctl(NETMAN_NETIF_IOCTL_GET_LINK_STATUS, NULL, 0, NULL, 0) == NETMAN_NETIF_ETH_LINK_STATE_UP);
}

int ethApplyIPConfig(void)
{
    t_ip_info ip_info;
    struct ip4_addr ipaddr, netmask, gw, dns, dns_curr;
    int result;

    if ((result = ps2ip_getconfig("sm0", &ip_info)) >= 0) {
        IP4_ADDR(&ipaddr, ps2_ip[0], ps2_ip[1], ps2_ip[2], ps2_ip[3]);
        IP4_ADDR(&netmask, ps2_netmask[0], ps2_netmask[1], ps2_netmask[2], ps2_netmask[3]);
        IP4_ADDR(&gw, ps2_gateway[0], ps2_gateway[1], ps2_gateway[2], ps2_gateway[3]);
        IP4_ADDR(&dns, ps2_dns[0], ps2_dns[1], ps2_dns[2], ps2_dns[3]);
        dns_curr = dns_getserver(0);

        //Check if it's the same. Otherwise, apply the new configuration.
        if ((ps2_ip_use_dhcp != ip_info.dhcp_enabled) || (!ps2_ip_use_dhcp &&
                                                          (!ip_addr_cmp(&ipaddr, (struct ip4_addr *)&ip_info.ipaddr) ||
                                                           !ip_addr_cmp(&netmask, (struct ip4_addr *)&ip_info.netmask) ||
                                                           !ip_addr_cmp(&gw, (struct ip4_addr *)&ip_info.gw) ||
                                                           !ip_addr_cmp(&dns, &dns_curr)))) {
            if (ps2_ip_use_dhcp) {
                IP4_ADDR((struct ip4_addr *)&ip_info.ipaddr, 169, 254, 0, 1);
                IP4_ADDR((struct ip4_addr *)&ip_info.netmask, 255, 255, 0, 0);
                IP4_ADDR((struct ip4_addr *)&ip_info.gw, 0, 0, 0, 0);
                IP4_ADDR(&dns, 0, 0, 0, 0);

                ip_info.dhcp_enabled = 1;
            } else {
                ip_addr_set((struct ip4_addr *)&ip_info.ipaddr, &ipaddr);
                ip_addr_set((struct ip4_addr *)&ip_info.netmask, &netmask);
                ip_addr_set((struct ip4_addr *)&ip_info.gw, &gw);

                ip_info.dhcp_enabled = 0;
            }

            dns_setserver(0, &dns);
            result = ps2ip_setconfig(&ip_info);
        } else
            result = 0;
    }

    return result;
}

int ethGetDHCPStatus(void)
{
    t_ip_info ip_info;
    int result;

    if ((result = ps2ip_getconfig("sm0", &ip_info)) >= 0) {
        if (ip_info.dhcp_enabled) {
            result = (ip_info.dhcp_status == DHCP_STATE_BOUND || (ip_info.dhcp_status == DHCP_STATE_OFF));
        } else
            result = -1;
    }

    return result;
}
