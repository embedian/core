/*
   Copyright (C) CFEngine AS

   This file is part of CFEngine 3 - written and maintained by CFEngine AS.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; version 3.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of CFEngine, the applicable Commercial Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/

#include <generic_agent.h>

#include <known_dirs.h>
#include <unix.h>
#include <eval_context.h>
#include <lastseen.h>
#include <crypto.h>
#include <files_names.h>
#include <promises.h>
#include <conversion.h>
#include <vars.h>
#include <client_code.h>
#include <communication.h>
#include <net.h>
#include <string_lib.h>
#include <rlist.h>
#include <scope.h>
#include <policy.h>
#include <audit.h>
#include <man.h>
#include <connection_info.h>
#include <addr_lib.h>

typedef enum
{
    RUNAGENT_CONTROL_HOSTS,
    RUNAGENT_CONTROL_PORT_NUMBER,
    RUNAGENT_CONTROL_FORCE_IPV4,
    RUNAGENT_CONTROL_TRUSTKEY,
    RUNAGENT_CONTROL_ENCRYPT,
    RUNAGENT_CONTROL_BACKGROUND,
    RUNAGENT_CONTROL_MAX_CHILD,
    RUNAGENT_CONTROL_OUTPUT_TO_FILE,
    RUNAGENT_CONTROL_OUTPUT_DIRECTORY,
    RUNAGENT_CONTROL_TIMEOUT,
    RUNAGENT_CONTROL_NONE
} RunagentControl;

static void ThisAgentInit(void);
static GenericAgentConfig *CheckOpts(int argc, char **argv);

static void KeepControlPromises(EvalContext *ctx, const Policy *policy);
static int HailServer(EvalContext *ctx, char *host);
static void SendClassData(AgentConnection *conn);
static void HailExec(AgentConnection *conn, char *peer, char *recvbuffer, char *sendbuffer);
static FILE *NewStream(char *name);

/*******************************************************************/
/* Command line options                                            */
/*******************************************************************/

static const char *const CF_RUNAGENT_SHORT_DESCRIPTION =
    "activate cf-agent on a remote host";

static const char *const CF_RUNAGENT_MANPAGE_LONG_DESCRIPTION =
    "cf-runagent connects to a list of running instances of "
    "cf-serverd. It allows foregoing the usual cf-execd schedule "
    "to activate cf-agent. Additionally, a user "
    "may send classes to be defined on the remote\n"
    "host. Two kinds of classes may be sent: classes to decide "
    "on which hosts cf-agent will be started, and classes that "
    "the user requests cf-agent should define on execution. "
    "The latter type is regulated by cf-serverd's role based access control.";

static const struct option OPTIONS[] =
{
    {"help", no_argument, 0, 'h'},
    {"background", optional_argument, 0, 'b'},
    {"debug", no_argument, 0, 'd'},
    {"verbose", no_argument, 0, 'v'},
    {"dry-run", no_argument, 0, 'n'},
    {"version", no_argument, 0, 'V'},
    {"file", required_argument, 0, 'f'},
    {"define-class", required_argument, 0, 'D'},
    {"select-class", required_argument, 0, 's'},
    {"inform", no_argument, 0, 'I'},
    {"remote-options", required_argument, 0, 'o'},
    {"diagnostic", no_argument, 0, 'x'},
    {"hail", required_argument, 0, 'H'},
    {"interactive", no_argument, 0, 'i'},
    {"timeout", required_argument, 0, 't'},
    {"legacy-output", no_argument, 0, 'l'},
    {"color", optional_argument, 0, 'C'},
    {NULL, 0, 0, '\0'}
};

static const char *const HINTS[] =
{
    "Print the help message",
    "Parallelize connections (50 by default)",
    "Enable debugging output",
    "Output verbose information about the behaviour of the agent",
    "All talk and no action mode - make no changes, only inform of promises not kept",
    "Output the version of the software",
    "Specify an alternative input file than the default",
    "Define a list of comma separated classes to be sent to a remote agent",
    "Define a list of comma separated classes to be used to select remote agents by constraint",
    "Print basic information about changes made to the system, i.e. promises repaired",
    "Pass options to a remote server process",
    "Activate internal diagnostics (developers only)",
    "Hail the following comma-separated lists of hosts, overriding default list",
    "Enable interactive mode for key trust",
    "Connection timeout, seconds",
    "Use legacy output format",
    "Enable colorized output. Possible values: 'always', 'auto', 'never'. If option is used, the default value is 'auto'",
    NULL
};

extern const ConstraintSyntax CFR_CONTROLBODY[];

int INTERACTIVE = false; /* GLOBAL_A */
int OUTPUT_TO_FILE = false; /* GLOBAL_P */
char OUTPUT_DIRECTORY[CF_BUFSIZE] = ""; /* GLOBAL_P */
int BACKGROUND = false; /* GLOBAL_P GLOBAL_A */
int MAXCHILD = 50; /* GLOBAL_P GLOBAL_A */
char REMOTE_AGENT_OPTIONS[CF_MAXVARSIZE] = ""; /* GLOBAL_A */

const Rlist *HOSTLIST = NULL; /* GLOBAL_P GLOBAL_A */
char SENDCLASSES[CF_MAXVARSIZE] = ""; /* GLOBAL_A */
char DEFINECLASSES[CF_MAXVARSIZE] = ""; /* GLOBAL_A */

/*****************************************************************************/

int main(int argc, char *argv[])
{
#if !defined(__MINGW32__)
    int count = 0;
    int status;
    int pid;
#endif

    EvalContext *ctx = EvalContextNew();

    GenericAgentConfig *config = CheckOpts(argc, argv);
    GenericAgentConfigApply(ctx, config);

    GenericAgentDiscoverContext(ctx, config);
    Policy *policy = GenericAgentLoadPolicy(ctx, config);

    ThisAgentInit();
    KeepControlPromises(ctx, policy);      // Set RUNATTR using copy

    if (BACKGROUND && INTERACTIVE)
    {
        Log(LOG_LEVEL_ERR, "You cannot specify background mode and interactive mode together");
        exit(EXIT_FAILURE);
    }

/* HvB */
    if (HOSTLIST)
    {
        const Rlist *rp = HOSTLIST;

        while (rp != NULL)
        {

#ifdef __MINGW32__
            if (BACKGROUND)
            {
                Log(LOG_LEVEL_VERBOSE,
                      "Windows does not support starting processes in the background - starting in foreground");
                BACKGROUND = false;
            }
#else
            if (BACKGROUND)     /* parallel */
            {
                if (count <= MAXCHILD)
                {
                    if (fork() == 0)    /* child process */
                    {
                        HailServer(ctx, RlistScalarValue(rp));
                        exit(EXIT_SUCCESS);
                    }
                    else        /* parent process */
                    {
                        rp = rp->next;
                        count++;
                    }
                }
                else
                {
                    pid = wait(&status);
                    Log(LOG_LEVEL_DEBUG, "child = %d, child number = %d", pid, count);
                    count--;
                }
            }
            else                /* serial */
#endif /* __MINGW32__ */
            {
                HailServer(ctx, RlistScalarValue(rp));
                rp = rp->next;
            }
        }                       /* end while */
    }                           /* end if HOSTLIST */

#ifndef __MINGW32__
    if (BACKGROUND)
    {
        Log(LOG_LEVEL_NOTICE, "Waiting for child processes to finish");
        while (count > 1)
        {
            pid = wait(&status);
            Log(LOG_LEVEL_VERBOSE, "Child %d ended, number %d", pid, count);
            count--;
        }
    }
#endif

    GenericAgentConfigDestroy(config);

    return 0;
}

/*******************************************************************/

static GenericAgentConfig *CheckOpts(int argc, char **argv)
{
    extern char *optarg;
    int optindex = 0;
    int c;
    GenericAgentConfig *config = GenericAgentConfigNewDefault(AGENT_TYPE_RUNAGENT);

    DEFINECLASSES[0] = '\0';
    SENDCLASSES[0] = '\0';

    while ((c = getopt_long(argc, argv, "t:q:db:vnKhIif:D:VSxo:s:MH:lC::", OPTIONS, &optindex)) != EOF)
    {
        switch ((char) c)
        {
        case 'l':
            LEGACY_OUTPUT = true;
            break;

        case 'f':
            GenericAgentConfigSetInputFile(config, GetInputDir(), optarg);
            MINUSF = true;
            break;

        case 'b':
            BACKGROUND = true;
            if (optarg)
            {
                MAXCHILD = atoi(optarg);
            }
            break;

        case 'd':
            LogSetGlobalLevel(LOG_LEVEL_DEBUG);
            break;

        case 'K':
            config->ignore_locks = true;
            break;

        case 's':
            strncpy(SENDCLASSES, optarg, CF_MAXVARSIZE);

            if (strlen(optarg) > CF_MAXVARSIZE)
            {
                Log(LOG_LEVEL_ERR, "Argument too long (-s)");
                exit(EXIT_FAILURE);
            }
            break;

        case 'D':
            strncpy(DEFINECLASSES, optarg, CF_MAXVARSIZE);

            if (strlen(optarg) > CF_MAXVARSIZE)
            {
                Log(LOG_LEVEL_ERR, "Argument too long (-D)");
                exit(EXIT_FAILURE);
            }
            break;

        case 'H':
            HOSTLIST = RlistFromSplitString(optarg, ',');
            break;

        case 'o':
            strncpy(REMOTE_AGENT_OPTIONS, optarg, CF_MAXVARSIZE);
            break;

        case 'I':
            LogSetGlobalLevel(LOG_LEVEL_INFO);
            break;

        case 'i':
            INTERACTIVE = true;
            break;

        case 'v':
            LogSetGlobalLevel(LOG_LEVEL_VERBOSE);
            break;

        case 'n':
            DONTDO = true;
            config->ignore_locks = true;
            break;

        case 't':
            CONNTIMEOUT = atoi(optarg);
            break;

        case 'V':
            {
                Writer *w = FileWriter(stdout);
                GenericAgentWriteVersion(w);
                FileWriterDetach(w);
            }
            exit(EXIT_SUCCESS);

        case 'h':
            {
                Writer *w = FileWriter(stdout);
                GenericAgentWriteHelp(w, "cf-runagent", OPTIONS, HINTS, true);
                FileWriterDetach(w);
            }
            exit(EXIT_SUCCESS);

        case 'M':
            {
                Writer *out = FileWriter(stdout);
                ManPageWrite(out, "cf-runagent", time(NULL),
                             CF_RUNAGENT_SHORT_DESCRIPTION,
                             CF_RUNAGENT_MANPAGE_LONG_DESCRIPTION,
                             OPTIONS, HINTS,
                             true);
                FileWriterDetach(out);
                exit(EXIT_SUCCESS);
            }

        case 'x':
            Log(LOG_LEVEL_ERR, "Self-diagnostic functionality is retired.");
            exit(EXIT_SUCCESS);

        case 'C':
            if (!GenericAgentConfigParseColor(config, optarg))
            {
                exit(EXIT_FAILURE);
            }
            break;

        default:
            {
                Writer *w = FileWriter(stdout);
                GenericAgentWriteHelp(w, "cf-runagent", OPTIONS, HINTS, true);
                FileWriterDetach(w);
            }
            exit(EXIT_FAILURE);

        }
    }

    if (!GenericAgentConfigParseArguments(config, argc - optind, argv + optind))
    {
        Log(LOG_LEVEL_ERR, "Too many arguments");
        exit(EXIT_FAILURE);
    }

    return config;
}

/*******************************************************************/

static void ThisAgentInit(void)
{
    umask(077);

    if (strstr(REMOTE_AGENT_OPTIONS, "--file") || strstr(REMOTE_AGENT_OPTIONS, "-f"))
    {
        Log(LOG_LEVEL_ERR,
              "The specified remote options include a useless --file option. The remote server has promised to ignore this, thus it is disallowed.");
        exit(EXIT_FAILURE);
    }
}

/********************************************************************/

static int HailServer(EvalContext *ctx, char *host)
{
    AgentConnection *conn;
    char sendbuffer[CF_BUFSIZE], recvbuffer[CF_BUFSIZE], peer[CF_MAXVARSIZE],
        digest[CF_MAXVARSIZE], user[CF_SMALLBUF];
    bool gotkey;
    char reply[8];

    FileCopy fc = {
        .portnumber = (unsigned short) ParseHostname(host, peer),
    };

    char ipaddr[CF_MAX_IP_LEN];
    if (Hostname2IPString(ipaddr, peer, sizeof(ipaddr)) == -1)
    {
        Log(LOG_LEVEL_ERR,
            "HailServer: ERROR, could not resolve '%s'", peer);
        return false;
    }

    Address2Hostkey(ipaddr, digest);
    GetCurrentUserName(user, CF_SMALLBUF);

    if (INTERACTIVE)
    {
        Log(LOG_LEVEL_VERBOSE, "Using interactive key trust...");

        gotkey = HavePublicKey(user, peer, digest) != NULL;

        if (!gotkey)
        {
            gotkey = HavePublicKey(user, ipaddr, digest) != NULL;
        }

        if (!gotkey)
        {
            printf("WARNING - You do not have a public key from host %s = %s\n",
                   host, ipaddr);
            printf("          Do you want to accept one on trust? (yes/no)\n\n--> ");

            while (true)
            {
                if (fgets(reply, sizeof(reply), stdin) == NULL)
                {
                    FatalError(ctx, "EOF trying to read answer from terminal");
                }

                if (Chop(reply, CF_EXPANDSIZE) == -1)
                {
                    Log(LOG_LEVEL_ERR, "Chop was called on a string that seemed to have no terminator");
                }

                if (strcmp(reply, "yes") == 0)
                {
                    printf("Will trust the key...\n");
                    fc.trustkey = true;
                    break;
                }
                else if (strcmp(reply, "no") == 0)
                {
                    printf("Will not trust the key...\n");
                    fc.trustkey = false;
                    break;
                }
                else
                {
                    printf("Please reply yes or no...(%s)\n", reply);
                }
            }
        }
    }

/* Continue */

#ifdef __MINGW32__

    if (LEGACY_OUTPUT)
    {
        Log(LOG_LEVEL_INFO, "...........................................................................");
        Log(LOG_LEVEL_INFO, " * Hailing %s : %u, with options \"%s\" (serial)", peer, fc.portnumber,
              REMOTE_AGENT_OPTIONS);
        Log(LOG_LEVEL_INFO, "...........................................................................");
    }
    else
    {
        Log(LOG_LEVEL_INFO, "Hailing '%s' : %u, with options '%s' (serial)", peer, fc.portnumber,
            REMOTE_AGENT_OPTIONS);
    }


#else /* !__MINGW32__ */

    if (BACKGROUND)
    {
        Log(LOG_LEVEL_INFO, "Hailing '%s' : %u, with options '%s' (parallel)", peer, fc.portnumber,
              REMOTE_AGENT_OPTIONS);
    }
    else
    {
        if (LEGACY_OUTPUT)
        {
            Log(LOG_LEVEL_INFO, "...........................................................................");
            Log(LOG_LEVEL_INFO, " * Hailing %s : %u, with options \"%s\" (serial)", peer, fc.portnumber,
                  REMOTE_AGENT_OPTIONS);
            Log(LOG_LEVEL_INFO, "...........................................................................");
        }
        else
        {
            Log(LOG_LEVEL_INFO, "Hailing '%s' : %u, with options '%s' (serial)", peer, fc.portnumber,
                  REMOTE_AGENT_OPTIONS);
        }
    }

#endif /* !__MINGW32__ */

    fc.servers = RlistFromSplitString(peer, '*');

    if (fc.servers == NULL || strcmp(RlistScalarValue(fc.servers), "localhost") == 0)
    {
        Log(LOG_LEVEL_INFO, "No hosts are registered to connect to");
        return false;
    }
    else
    {
        int err = 0;
        conn = NewServerConnection(fc, false, &err, -1);

        if (conn == NULL)
        {
            RlistDestroy(fc.servers);
            Log(LOG_LEVEL_VERBOSE, "No suitable server responded to hail");
            return false;
        }
    }

/* Check trust interaction*/

    HailExec(conn, peer, recvbuffer, sendbuffer);

    RlistDestroy(fc.servers);

    return true;
}

/********************************************************************/
/* Level 2                                                          */
/********************************************************************/

static void KeepControlPromises(EvalContext *ctx, const Policy *policy)
{
    Seq *constraints = ControlBodyConstraints(policy, AGENT_TYPE_RUNAGENT);
    if (constraints)
    {
        for (size_t i = 0; i < SeqLength(constraints); i++)
        {
            Constraint *cp = SeqAt(constraints, i);

            if (!IsDefinedClass(ctx, cp->classes, NULL))
            {
                continue;
            }

            VarRef *ref = VarRefParseFromScope(cp->lval, "control_runagent");
            const void *value = EvalContextVariableGet(ctx, ref, NULL);
            VarRefDestroy(ref);

            if (!value)
            {
                Log(LOG_LEVEL_ERR, "Unknown lval '%s' in runagent control body", cp->lval);
                continue;
            }

            if (strcmp(cp->lval, CFR_CONTROLBODY[RUNAGENT_CONTROL_FORCE_IPV4].lval) == 0)
            {
                continue;
            }

            if (strcmp(cp->lval, CFR_CONTROLBODY[RUNAGENT_CONTROL_TRUSTKEY].lval) == 0)
            {
                continue;
            }

            if (strcmp(cp->lval, CFR_CONTROLBODY[RUNAGENT_CONTROL_ENCRYPT].lval) == 0)
            {
                continue;
            }

            if (strcmp(cp->lval, CFR_CONTROLBODY[RUNAGENT_CONTROL_PORT_NUMBER].lval) == 0)
            {
                continue;
            }

            if (strcmp(cp->lval, CFR_CONTROLBODY[RUNAGENT_CONTROL_BACKGROUND].lval) == 0)
            {
                /*
                 * Only process this option if are is no -b or -i options specified on
                 * command line.
                 */
                if (BACKGROUND || INTERACTIVE)
                {
                    Log(LOG_LEVEL_WARNING,
                          "'background_children' setting from 'body runagent control' is overridden by command-line option.");
                }
                else
                {
                    BACKGROUND = BooleanFromString(value);
                }
                continue;
            }

            if (strcmp(cp->lval, CFR_CONTROLBODY[RUNAGENT_CONTROL_MAX_CHILD].lval) == 0)
            {
                MAXCHILD = (short) IntFromString(value);
                continue;
            }

            if (strcmp(cp->lval, CFR_CONTROLBODY[RUNAGENT_CONTROL_OUTPUT_TO_FILE].lval) == 0)
            {
                OUTPUT_TO_FILE = BooleanFromString(value);
                continue;
            }

            if (strcmp(cp->lval, CFR_CONTROLBODY[RUNAGENT_CONTROL_OUTPUT_DIRECTORY].lval) == 0)
            {
                if (IsAbsPath(value))
                {
                    strncpy(OUTPUT_DIRECTORY, value, CF_BUFSIZE - 1);
                    Log(LOG_LEVEL_VERBOSE, "Setting output direcory to '%s'", OUTPUT_DIRECTORY);
                }
                continue;
            }

            if (strcmp(cp->lval, CFR_CONTROLBODY[RUNAGENT_CONTROL_TIMEOUT].lval) == 0)
            {
                continue;
            }

            if (strcmp(cp->lval, CFR_CONTROLBODY[RUNAGENT_CONTROL_HOSTS].lval) == 0)
            {
                if (HOSTLIST == NULL)       // Don't override if command line setting
                {
                    HOSTLIST = value;
                }

                continue;
            }
        }
    }

    const char *expire_after = EvalContextVariableControlCommonGet(ctx, COMMON_CONTROL_LASTSEEN_EXPIRE_AFTER);
    if (expire_after)
    {
        LASTSEENEXPIREAFTER = IntFromString(expire_after) * 60;
    }

}

static void SendClassData(AgentConnection *conn)
{
    Rlist *classes, *rp;
    char sendbuffer[CF_BUFSIZE];

    classes = RlistFromSplitRegex(SENDCLASSES, "[,: ]", 99, false);

    for (rp = classes; rp != NULL; rp = rp->next)
    {
        if (SendTransaction(conn->conn_info, RlistScalarValue(rp), 0, CF_DONE) == -1)
        {
            Log(LOG_LEVEL_ERR, "Transaction failed. (send: %s)", GetErrorStr());
            return;
        }
    }

    snprintf(sendbuffer, CF_MAXVARSIZE, "%s", CFD_TERMINATOR);

    if (SendTransaction(conn->conn_info, sendbuffer, 0, CF_DONE) == -1)
    {
        Log(LOG_LEVEL_ERR, "Transaction failed. (send: %s)", GetErrorStr());
        return;
    }
}

/********************************************************************/

static void HailExec(AgentConnection *conn, char *peer, char *recvbuffer, char *sendbuffer)
{
    FILE *fp = stdout;
    char *sp;
    int n_read;

    if (strlen(DEFINECLASSES))
    {
        snprintf(sendbuffer, CF_BUFSIZE, "EXEC %s -D%s", REMOTE_AGENT_OPTIONS, DEFINECLASSES);
    }
    else
    {
        snprintf(sendbuffer, CF_BUFSIZE, "EXEC %s", REMOTE_AGENT_OPTIONS);
    }

    if (SendTransaction(conn->conn_info, sendbuffer, 0, CF_DONE) == -1)
    {
        Log(LOG_LEVEL_ERR, "Transmission rejected. (send: %s)", GetErrorStr());
        DisconnectServer(conn, false);
        return;
    }

    fp = NewStream(peer);
    SendClassData(conn);

    while (true)
    {
        memset(recvbuffer, 0, CF_BUFSIZE);

        if ((n_read = ReceiveTransaction(conn->conn_info, recvbuffer, NULL)) == -1)
        {
            return;
        }

        if (n_read == 0)
        {
            break;
        }

        if (strlen(recvbuffer) == 0)
        {
            continue;
        }

        if ((sp = strstr(recvbuffer, CFD_TERMINATOR)) != NULL)
        {
            break;
        }

        if ((sp = strstr(recvbuffer, "BAD:")) != NULL)
        {
            fprintf(fp, "%s> !! %s\n", VPREFIX, recvbuffer + 4);
            continue;
        }

        if (strstr(recvbuffer, "too soon"))
        {
            fprintf(fp, "%s> !! %s\n", VPREFIX, recvbuffer);
            continue;
        }

        fprintf(fp, "%s> -> %s", VPREFIX, recvbuffer);
    }

    if (fp != stdout)
    {
        fclose(fp);
    }
    DisconnectServer(conn, false);
}

/********************************************************************/
/* Level                                                            */
/********************************************************************/

static FILE *NewStream(char *name)
{
    FILE *fp;
    char filename[CF_BUFSIZE];

    if (OUTPUT_DIRECTORY[0] != '\0')
    {
        snprintf(filename, CF_BUFSIZE, "%s/%s_runagent.out", OUTPUT_DIRECTORY, name);
    }
    else
    {
        snprintf(filename, CF_BUFSIZE, "%s/outputs/%s_runagent.out", CFWORKDIR, name);
    }

    if (OUTPUT_TO_FILE)
    {
        printf("Opening file...%s\n", filename);

        if ((fp = fopen(filename, "w")) == NULL)
        {
            Log(LOG_LEVEL_ERR, "Unable to open file '%s'", filename);
            fp = stdout;
        }
    }
    else
    {
        fp = stdout;
    }

    return fp;
}
