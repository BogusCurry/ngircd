/*
 * ngIRCd -- The Next Generation IRC Daemon
 * Copyright (c)2001,2002 Alexander Barton (alex@barton.de)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * Please read the file COPYING, README and AUTHORS for more information.
 *
 * Configuration management (reading, parsing & validation)
 */


#include "portab.h"

static char UNUSED id[] = "$Id: conf.c,v 1.77.2.1 2005/10/11 19:28:47 alex Exp $";

#include "imp.h"
#include <assert.h>
#include <errno.h>
#ifdef PROTOTYPES
#	include <stdarg.h>
#else
#	include <varargs.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef HAVE_CTYPE_H
# include <ctype.h>
#endif

#include "ngircd.h"
#include "conn.h"
#include "client.h"
#include "defines.h"
#include "log.h"
#include "resolve.h"
#include "tool.h"

#include "exp.h"
#include "conf.h"


LOCAL bool Use_Log = true;
LOCAL CONF_SERVER New_Server;
LOCAL int New_Server_Idx;


LOCAL void Set_Defaults PARAMS(( bool InitServers ));
LOCAL void Read_Config PARAMS(( void ));
LOCAL void Validate_Config PARAMS(( bool TestOnly ));

LOCAL void Handle_GLOBAL PARAMS(( int Line, char *Var, char *Arg ));
LOCAL void Handle_OPERATOR PARAMS(( int Line, char *Var, char *Arg ));
LOCAL void Handle_SERVER PARAMS(( int Line, char *Var, char *Arg ));
LOCAL void Handle_CHANNEL PARAMS(( int Line, char *Var, char *Arg ));

LOCAL void Config_Error PARAMS(( const int Level, const char *Format, ... ));

LOCAL void Config_Error_NaN PARAMS(( const int LINE, const char *Value ));
LOCAL void Config_Error_TooLong PARAMS(( const int LINE, const char *Value ));

LOCAL void Init_Server_Struct PARAMS(( CONF_SERVER *Server ));


GLOBAL void
Conf_Init( void )
{
	Set_Defaults( true );
	Read_Config( );
	Validate_Config( false );
} /* Config_Init */


GLOBAL void
Conf_Rehash( void )
{
	Set_Defaults( false );
	Read_Config( );
	Validate_Config( false );
} /* Config_Rehash */


GLOBAL int
Conf_Test( void )
{
	/* Read configuration, validate and output it. */

	struct passwd *pwd;
	struct group *grp;
	int i;

	Use_Log = false;
	Set_Defaults( true );

	Read_Config( );
	Validate_Config( true );

	/* If stdin and stdout ("you can read our nice message and we can
	 * read in your keypress") are valid tty's, wait for a key: */
	if( isatty( fileno( stdin )) && isatty( fileno( stdout )))
	{
		puts( "OK, press enter to see a dump of your service configuration ..." );
		getchar( );
	}
	else puts( "Ok, dump of your server configuration follows:\n" );

	puts( "[GLOBAL]" );
	printf( "  Name = %s\n", Conf_ServerName );
	printf( "  Info = %s\n", Conf_ServerInfo );
	printf( "  Password = %s\n", Conf_ServerPwd );
	printf( "  AdminInfo1 = %s\n", Conf_ServerAdmin1 );
	printf( "  AdminInfo2 = %s\n", Conf_ServerAdmin2 );
	printf( "  AdminEMail = %s\n", Conf_ServerAdminMail );
	printf( "  MotdFile = %s\n", Conf_MotdFile );
	printf( "  MotdPhrase = %s\n", Conf_MotdPhrase );
	printf( "  ChrootDir = %s\n", Conf_Chroot );
	printf( "  PidFile = %s\n", Conf_PidFile);
	printf( "  Ports = " );
	for( i = 0; i < Conf_ListenPorts_Count; i++ )
	{
		if( i != 0 ) printf( ", " );
		printf( "%u", (unsigned int) Conf_ListenPorts[i] );
	}
	puts( "" );
	printf( "  Listen = %s\n", Conf_ListenAddress );
	pwd = getpwuid( Conf_UID );
	if( pwd ) printf( "  ServerUID = %s\n", pwd->pw_name );
	else printf( "  ServerUID = %ld\n", (long)Conf_UID );
	grp = getgrgid( Conf_GID );
	if( grp ) printf( "  ServerGID = %s\n", grp->gr_name );
	else printf( "  ServerGID = %ld\n", (long)Conf_GID );
	printf( "  PingTimeout = %d\n", Conf_PingTimeout );
	printf( "  PongTimeout = %d\n", Conf_PongTimeout );
	printf( "  ConnectRetry = %d\n", Conf_ConnectRetry );
	printf( "  OperCanUseMode = %s\n", Conf_OperCanMode == true? "yes" : "no" );
	printf( "  OperServerMode = %s\n", Conf_OperServerMode == true? "yes" : "no" );
	if( Conf_MaxConnections > 0 ) printf( "  MaxConnections = %ld\n", Conf_MaxConnections );
	else printf( "  MaxConnections = -1\n" );
	if( Conf_MaxConnectionsIP > 0 ) printf( "  MaxConnectionsIP = %d\n", Conf_MaxConnectionsIP );
	else printf( "  MaxConnectionsIP = -1\n" );
	if( Conf_MaxJoins > 0 ) printf( "  MaxJoins = %d\n", Conf_MaxJoins );
	else printf( "  MaxJoins = -1\n" );
	puts( "" );

	for( i = 0; i < Conf_Oper_Count; i++ )
	{
		if( ! Conf_Oper[i].name[0] ) continue;
		
		/* Valid "Operator" section */
		puts( "[OPERATOR]" );
		printf( "  Name = %s\n", Conf_Oper[i].name );
		printf( "  Password = %s\n", Conf_Oper[i].pwd );
		if ( Conf_Oper[i].mask ) printf( "  Mask = %s\n", Conf_Oper[i].mask );
		puts( "" );
	}

	for( i = 0; i < MAX_SERVERS; i++ )
	{
		if( ! Conf_Server[i].name[0] ) continue;
		
		/* Valid "Server" section */
		puts( "[SERVER]" );
		printf( "  Name = %s\n", Conf_Server[i].name );
		printf( "  Host = %s\n", Conf_Server[i].host );
		printf( "  Port = %d\n", Conf_Server[i].port );
		printf( "  MyPassword = %s\n", Conf_Server[i].pwd_in );
		printf( "  PeerPassword = %s\n", Conf_Server[i].pwd_out );
		printf( "  Group = %d\n", Conf_Server[i].group );
		puts( "" );
	}

	for( i = 0; i < Conf_Channel_Count; i++ )
	{
		if( ! Conf_Channel[i].name[0] ) continue;
		
		/* Valid "Channel" section */
		puts( "[CHANNEL]" );
		printf( "  Name = %s\n", Conf_Channel[i].name );
		printf( "  Modes = %s\n", Conf_Channel[i].modes );
		printf( "  Topic = %s\n", Conf_Channel[i].topic );
		puts( "" );
	}
	
	return 0;
} /* Conf_Test */


GLOBAL void
Conf_UnsetServer( CONN_ID Idx )
{
	/* Set next time for next connection attempt, if this is a server
	 * link that is (still) configured here. If the server is set as
	 * "once", delete it from our configuration.
	 * Non-Server-Connections will be silently ignored. */

	int i;
	time_t t;

	/* Check all our configured servers */
	for( i = 0; i < MAX_SERVERS; i++ )
	{
		if( Conf_Server[i].conn_id != Idx ) continue;

		/* Gotcha! Mark server configuration as "unused": */
		Conf_Server[i].conn_id = NONE;

		if( Conf_Server[i].flags & CONF_SFLAG_ONCE )
		{
			/* Delete configuration here */
			Init_Server_Struct( &Conf_Server[i] );
		}
		else
		{
			/* Set time for next connect attempt */
			t = time(NULL);
			if (Conf_Server[i].lasttry < t - Conf_ConnectRetry) {
				/* The connection has been "long", so we don't
				 * require the next attempt to be delayed. */
				Conf_Server[i].lasttry =
					t - Conf_ConnectRetry + RECONNECT_DELAY;
			} else
				Conf_Server[i].lasttry = t;
		}
	}
} /* Conf_UnsetServer */


GLOBAL void
Conf_SetServer( int ConfServer, CONN_ID Idx )
{
	/* Set connection for specified configured server */

	assert( ConfServer > NONE );
	assert( Idx > NONE );

	Conf_Server[ConfServer].conn_id = Idx;
} /* Conf_SetServer */


GLOBAL int
Conf_GetServer( CONN_ID Idx )
{
	/* Get index of server in configuration structure */
	
	int i = 0;
	
	assert( Idx > NONE );

	for( i = 0; i < MAX_SERVERS; i++ )
	{
		if( Conf_Server[i].conn_id == Idx ) return i;
	}
	return NONE;
} /* Conf_GetServer */


GLOBAL bool
Conf_EnableServer( char *Name, UINT16 Port )
{
	/* Enable specified server and adjust port */

	int i;

	assert( Name != NULL );

	for( i = 0; i < MAX_SERVERS; i++ )
	{
		if( strcasecmp( Conf_Server[i].name, Name ) == 0 )
		{
			/* Gotcha! Set port and enable server: */
			Conf_Server[i].port = Port;
			Conf_Server[i].flags &= ~CONF_SFLAG_DISABLED;
			return true;
		}
	}
	return false;
} /* Conf_EnableServer */


GLOBAL bool
Conf_DisableServer( char *Name )
{
	/* Enable specified server and adjust port */

	int i;

	assert( Name != NULL );

	for( i = 0; i < MAX_SERVERS; i++ )
	{
		if( strcasecmp( Conf_Server[i].name, Name ) == 0 )
		{
			/* Gotcha! Disable and disconnect server: */
			Conf_Server[i].flags |= CONF_SFLAG_DISABLED;
			if( Conf_Server[i].conn_id > NONE ) Conn_Close( Conf_Server[i].conn_id, NULL, "Server link terminated on operator request", true);
			return true;
		}
	}
	return false;
} /* Conf_DisableServer */


GLOBAL bool
Conf_AddServer( char *Name, UINT16 Port, char *Host, char *MyPwd, char *PeerPwd )
{
	/* Add new server to configuration */

	int i;

	assert( Name != NULL );
	assert( Host != NULL );
	assert( MyPwd != NULL );
	assert( PeerPwd != NULL );

	/* Search unused item in server configuration structure */
	for( i = 0; i < MAX_SERVERS; i++ )
	{
		/* Is this item used? */
		if( ! Conf_Server[i].name[0] ) break;
	}
	if( i >= MAX_SERVERS ) return false;

	Init_Server_Struct( &Conf_Server[i] );
	strlcpy( Conf_Server[i].name, Name, sizeof( Conf_Server[i].name ));
	strlcpy( Conf_Server[i].host, Host, sizeof( Conf_Server[i].host ));
	strlcpy( Conf_Server[i].pwd_out, MyPwd, sizeof( Conf_Server[i].pwd_out ));
	strlcpy( Conf_Server[i].pwd_in, PeerPwd, sizeof( Conf_Server[i].pwd_in ));
	Conf_Server[i].port = Port;
	Conf_Server[i].flags = CONF_SFLAG_ONCE;
	
	return true;
} /* Conf_AddServer */


LOCAL void
Set_Defaults( bool InitServers )
{
	/* Initialize configuration variables with default values. */

	int i;

	strcpy( Conf_ServerName, "" );
	snprintf( Conf_ServerInfo, sizeof Conf_ServerInfo, "%s %s", PACKAGE_NAME, PACKAGE_VERSION );
	strcpy( Conf_ServerPwd, "" );

	strcpy( Conf_ServerAdmin1, "" );
	strcpy( Conf_ServerAdmin2, "" );
	strcpy( Conf_ServerAdminMail, "" );

	strlcpy( Conf_MotdFile, SYSCONFDIR, sizeof( Conf_MotdFile ));
	strlcat( Conf_MotdFile, MOTD_FILE, sizeof( Conf_MotdFile ));

	strlcpy( Conf_MotdPhrase, MOTD_PHRASE, sizeof( Conf_MotdPhrase ));

	strlcpy( Conf_Chroot, CHROOT_DIR, sizeof( Conf_Chroot ));

	strlcpy( Conf_PidFile, PID_FILE, sizeof( Conf_PidFile ));

	Conf_ListenPorts_Count = 0;
	strcpy( Conf_ListenAddress, "" );

	Conf_UID = Conf_GID = 0;
	
	Conf_PingTimeout = 120;
	Conf_PongTimeout = 20;

	Conf_ConnectRetry = 60;

	Conf_Oper_Count = 0;
	Conf_Channel_Count = 0;

	Conf_OperCanMode = false;
	Conf_OperServerMode = false;
	
	Conf_MaxConnections = -1;
	Conf_MaxConnectionsIP = 5;
	Conf_MaxJoins = 10;

	/* Initialize server configuration structures */
	if( InitServers ) for( i = 0; i < MAX_SERVERS; Init_Server_Struct( &Conf_Server[i++] ));
} /* Set_Defaults */


LOCAL void
Read_Config( void )
{
	/* Read configuration file. */

	char section[LINE_LEN], str[LINE_LEN], *var, *arg, *ptr;
	int line, i, n;
	FILE *fd;

	/* Open configuration file */
	fd = fopen( NGIRCd_ConfFile, "r" );
	if( ! fd )
	{
		/* No configuration file found! */
		Config_Error( LOG_ALERT, "Can't read configuration \"%s\": %s", NGIRCd_ConfFile, strerror( errno ));
		Config_Error( LOG_ALERT, "%s exiting due to fatal errors!", PACKAGE_NAME );
		exit( 1 );
	}

	Config_Error( LOG_INFO, "Reading configuration from \"%s\" ...", NGIRCd_ConfFile );

	/* Clean up server configuration structure: mark all already
	 * configured servers as "once" so that they are deleted
	 * after the next disconnect and delete all unused servers.
	 * And delete all servers which are "duplicates" of servers
	 * that are already marked as "once" (such servers have been
	 * created by the last rehash but are now useless). */
	for( i = 0; i < MAX_SERVERS; i++ )
	{
		if( Conf_Server[i].conn_id == NONE ) Init_Server_Struct( &Conf_Server[i] );
		else
		{
			/* This structure is in use ... */
			if( Conf_Server[i].flags & CONF_SFLAG_ONCE )
			{
				/* Check for duplicates */
				for( n = 0; n < MAX_SERVERS; n++ )
				{
					if( n == i ) continue;

					if( Conf_Server[i].conn_id == Conf_Server[n].conn_id )
					{
						Init_Server_Struct( &Conf_Server[n] );
						Log( LOG_DEBUG, "Deleted unused duplicate server %d (kept %d).", n, i );
					}
				}
			}
			else
			{
				/* Mark server as "once" */
				Conf_Server[i].flags |= CONF_SFLAG_ONCE;
				Log( LOG_DEBUG, "Marked server %d as \"once\"", i );
			}
		}
	}

	/* Initialize variables */
	line = 0;
	strcpy( section, "" );
	Init_Server_Struct( &New_Server );
	New_Server_Idx = NONE;

	/* Read configuration file */
	while( true )
	{
		if( ! fgets( str, LINE_LEN, fd )) break;
		ngt_TrimStr( str );
		line++;

		/* Skip comments and empty lines */
		if( str[0] == ';' || str[0] == '#' || str[0] == '\0' ) continue;

		/* Is this the beginning of a new section? */
		if(( str[0] == '[' ) && ( str[strlen( str ) - 1] == ']' ))
		{
			strlcpy( section, str, sizeof( section ));
			if( strcasecmp( section, "[GLOBAL]" ) == 0 ) continue;
			if( strcasecmp( section, "[OPERATOR]" ) == 0 )
			{
				if( Conf_Oper_Count + 1 > MAX_OPERATORS ) Config_Error( LOG_ERR, "Too many operators configured." );
				else
				{
					/* Initialize new operator structure */
					Conf_Oper[Conf_Oper_Count].name[0] = '\0';
					Conf_Oper[Conf_Oper_Count].pwd[0] = '\0';
					if (Conf_Oper[Conf_Oper_Count].mask) {
						free(Conf_Oper[Conf_Oper_Count].mask );
						Conf_Oper[Conf_Oper_Count].mask = NULL;
					}
					Conf_Oper_Count++;
				}
				continue;
			}
			if( strcasecmp( section, "[SERVER]" ) == 0 )
			{
				/* Check if there is already a server to add */
				if( New_Server.name[0] )
				{
					/* Copy data to "real" server structure */
					assert( New_Server_Idx > NONE );
					Conf_Server[New_Server_Idx] = New_Server;
				}

				/* Re-init structure for new server */
				Init_Server_Struct( &New_Server );

				/* Search unused item in server configuration structure */
				for( i = 0; i < MAX_SERVERS; i++ )
				{
					/* Is this item used? */
					if( ! Conf_Server[i].name[0] ) break;
				}
				if( i >= MAX_SERVERS )
				{
					/* Oops, no free item found! */
					Config_Error( LOG_ERR, "Too many servers configured." );
					New_Server_Idx = NONE;
				}
				else New_Server_Idx = i;
				continue;
			}
			if( strcasecmp( section, "[CHANNEL]" ) == 0 )
			{
				if( Conf_Channel_Count + 1 > MAX_DEFCHANNELS ) Config_Error( LOG_ERR, "Too many pre-defined channels configured." );
				else
				{
					/* Initialize new channel structure */
					strcpy( Conf_Channel[Conf_Channel_Count].name, "" );
					strcpy( Conf_Channel[Conf_Channel_Count].modes, "" );
					strcpy( Conf_Channel[Conf_Channel_Count].topic, "" );
					Conf_Channel_Count++;
				}
				continue;
			}
			Config_Error( LOG_ERR, "%s, line %d: Unknown section \"%s\"!", NGIRCd_ConfFile, line, section );
			section[0] = 0x1;
		}
		if( section[0] == 0x1 ) continue;

		/* Split line into variable name and parameters */
		ptr = strchr( str, '=' );
		if( ! ptr )
		{
			Config_Error( LOG_ERR, "%s, line %d: Syntax error!", NGIRCd_ConfFile, line );
			continue;
		}
		*ptr = '\0';
		var = str; ngt_TrimStr( var );
		arg = ptr + 1; ngt_TrimStr( arg );

		if( strcasecmp( section, "[GLOBAL]" ) == 0 ) Handle_GLOBAL( line, var, arg );
		else if( strcasecmp( section, "[OPERATOR]" ) == 0 ) Handle_OPERATOR( line, var, arg );
		else if( strcasecmp( section, "[SERVER]" ) == 0 ) Handle_SERVER( line, var, arg );
		else if( strcasecmp( section, "[CHANNEL]" ) == 0 ) Handle_CHANNEL( line, var, arg );
		else Config_Error( LOG_ERR, "%s, line %d: Variable \"%s\" outside section!", NGIRCd_ConfFile, line, var );
	}

	/* Close configuration file */
	fclose( fd );

	/* Check if there is still a server to add */
	if( New_Server.name[0] )
	{
		/* Copy data to "real" server structure */
		assert( New_Server_Idx > NONE );
		Conf_Server[New_Server_Idx] = New_Server;
	}
	
	/* If there are no ports configured use the default: 6667 */
	if( Conf_ListenPorts_Count < 1 )
	{
		Conf_ListenPorts_Count = 1;
		Conf_ListenPorts[0] = 6667;
	}
} /* Read_Config */


LOCAL bool
Check_ArgIsTrue( const char *Arg )
{
	if( strcasecmp( Arg, "yes" ) == 0 ) return true;
	if( strcasecmp( Arg, "true" ) == 0 ) return true;
	if( atoi( Arg ) != 0 ) return true;

	return false;
} /* Check_ArgIsTrue */


LOCAL void
Handle_GLOBAL( int Line, char *Var, char *Arg )
{
	struct passwd *pwd;
	struct group *grp;
	char *ptr;
	long port;
	
	assert( Line > 0 );
	assert( Var != NULL );
	assert( Arg != NULL );
	
	if( strcasecmp( Var, "Name" ) == 0 )
	{
		/* Server name */
		if( strlcpy( Conf_ServerName, Arg, sizeof( Conf_ServerName )) >= sizeof( Conf_ServerName ))
			Config_Error_TooLong( Line, Var );

		return;
	}
	if( strcasecmp( Var, "Info" ) == 0 )
	{
		/* Info text of server */
		if( strlcpy( Conf_ServerInfo, Arg, sizeof( Conf_ServerInfo )) >= sizeof( Conf_ServerInfo ))
			Config_Error_TooLong ( Line, Var );

		return;
	}
	if( strcasecmp( Var, "Password" ) == 0 )
	{
		/* Global server password */
		if( strlcpy( Conf_ServerPwd, Arg, sizeof( Conf_ServerPwd )) >= sizeof( Conf_ServerPwd ))
			Config_Error_TooLong( Line, Var );

		return;
	}
	if( strcasecmp( Var, "AdminInfo1" ) == 0 )
	{
		/* Administrative info #1 */
		if( strlcpy( Conf_ServerAdmin1, Arg, sizeof( Conf_ServerAdmin1 )) >= sizeof( Conf_ServerAdmin1 )) Config_Error_TooLong ( Line, Var );
		return;
	}
	if( strcasecmp( Var, "AdminInfo2" ) == 0 )
	{
		/* Administrative info #2 */
		if( strlcpy( Conf_ServerAdmin2, Arg, sizeof( Conf_ServerAdmin2 )) >= sizeof( Conf_ServerAdmin2 )) Config_Error_TooLong ( Line, Var );
		return;
	}
	if( strcasecmp( Var, "AdminEMail" ) == 0 )
	{
		/* Administrative email contact */
		if( strlcpy( Conf_ServerAdminMail, Arg, sizeof( Conf_ServerAdminMail )) >= sizeof( Conf_ServerAdminMail )) Config_Error_TooLong( Line, Var );
		return;
	}
	if( strcasecmp( Var, "Ports" ) == 0 )
	{
		/* Ports on that the server should listen. More port numbers
		 * must be separated by "," */
		ptr = strtok( Arg, "," );
		while( ptr )
		{
			ngt_TrimStr( ptr );
			port = atol( ptr );
			if( Conf_ListenPorts_Count + 1 > MAX_LISTEN_PORTS ) Config_Error( LOG_ERR, "Too many listen ports configured. Port %ld ignored.", port );
			else
			{
				if( port > 0 && port < 0xFFFF ) Conf_ListenPorts[Conf_ListenPorts_Count++] = (UINT16)port;
				else Config_Error( LOG_ERR, "%s, line %d (section \"Global\"): Illegal port number %ld!", NGIRCd_ConfFile, Line, port );
			}
			ptr = strtok( NULL, "," );
		}
		return;
	}
	if( strcasecmp( Var, "MotdFile" ) == 0 )
	{
		/* "Message of the day" (MOTD) file */
		if( strlcpy( Conf_MotdFile, Arg, sizeof( Conf_MotdFile )) >= sizeof( Conf_MotdFile ))
			Config_Error_TooLong( Line, Var );

		return;
	}
	if( strcasecmp( Var, "MotdPhrase" ) == 0 )
	{
		/* "Message of the day" phrase (instead of file) */
		if( strlcpy( Conf_MotdPhrase, Arg, sizeof( Conf_MotdPhrase )) >= sizeof( Conf_MotdPhrase ))
			Config_Error_TooLong( Line, Var );

		return;
	}
	if( strcasecmp( Var, "ChrootDir" ) == 0 )
	{
		/* directory for chroot() */
		if( strlcpy( Conf_Chroot, Arg, sizeof( Conf_Chroot )) >= sizeof( Conf_Chroot ))
			Config_Error_TooLong( Line, Var );

		return;
	}

	if ( strcasecmp( Var, "PidFile" ) == 0 )
	{
		/* name of pidfile */
		if( strlcpy( Conf_PidFile, Arg, sizeof( Conf_PidFile )) >= sizeof( Conf_PidFile ))
			Config_Error_TooLong( Line, Var );

		return;
	}

	if( strcasecmp( Var, "ServerUID" ) == 0 )
	{
		/* UID the daemon should switch to */
		pwd = getpwnam( Arg );
		if( pwd ) Conf_UID = pwd->pw_uid;
		else
		{
#ifdef HAVE_ISDIGIT
			if( ! isdigit( (int)*Arg )) Config_Error_NaN( Line, Var );
			else
#endif
			Conf_UID = (unsigned int)atoi( Arg );
		}
		return;
	}
	if( strcasecmp( Var, "ServerGID" ) == 0 )
	{
		/* GID the daemon should use */
		grp = getgrnam( Arg );
		if( grp ) Conf_GID = grp->gr_gid;
		else
		{
#ifdef HAVE_ISDIGIT
			if( ! isdigit( (int)*Arg )) Config_Error_NaN( Line, Var );
			else
#endif
			Conf_GID = (unsigned int)atoi( Arg );
		}
		return;
	}
	if( strcasecmp( Var, "PingTimeout" ) == 0 )
	{
		/* PING timeout */
		Conf_PingTimeout = atoi( Arg );
		if( Conf_PingTimeout < 5 )
		{
			Config_Error( LOG_WARNING, "%s, line %d: Value of \"PingTimeout\" too low!", NGIRCd_ConfFile, Line );
			Conf_PingTimeout = 5;
		}
		return;
	}
	if( strcasecmp( Var, "PongTimeout" ) == 0 )
	{
		/* PONG timeout */
		Conf_PongTimeout = atoi( Arg );
		if( Conf_PongTimeout < 5 )
		{
			Config_Error( LOG_WARNING, "%s, line %d: Value of \"PongTimeout\" too low!", NGIRCd_ConfFile, Line );
			Conf_PongTimeout = 5;
		}
		return;
	}
	if( strcasecmp( Var, "ConnectRetry" ) == 0 )
	{
		/* Seconds between connection attempts to other servers */
		Conf_ConnectRetry = atoi( Arg );
		if( Conf_ConnectRetry < 5 )
		{
			Config_Error( LOG_WARNING, "%s, line %d: Value of \"ConnectRetry\" too low!", NGIRCd_ConfFile, Line );
			Conf_ConnectRetry = 5;
		}
		return;
	}
	if( strcasecmp( Var, "OperCanUseMode" ) == 0 )
	{
		/* Are IRC operators allowed to use MODE in channels they aren't Op in? */
		Conf_OperCanMode = Check_ArgIsTrue( Arg );
		return;
	}
	if( strcasecmp( Var, "OperServerMode" ) == 0 )
	{
		/* Mask IRC operator as if coming from the server? (ircd-irc2 compat hack) */
		Conf_OperServerMode = Check_ArgIsTrue( Arg );
		return;
	}
	if( strcasecmp( Var, "MaxConnections" ) == 0 )
	{
		/* Maximum number of connections. Values <= 0 are equal to "no limit". */
#ifdef HAVE_ISDIGIT
		if( ! isdigit( (int)*Arg )) Config_Error_NaN( Line, Var);
		else
#endif
		Conf_MaxConnections = atol( Arg );
		return;
	}
	if( strcasecmp( Var, "MaxConnectionsIP" ) == 0 )
	{
		/* Maximum number of simoultanous connections from one IP. Values <= 0 are equal to "no limit". */
#ifdef HAVE_ISDIGIT
		if( ! isdigit( (int)*Arg )) Config_Error_NaN( Line, Var );
		else
#endif
		Conf_MaxConnectionsIP = atoi( Arg );
		return;
	}
	if( strcasecmp( Var, "MaxJoins" ) == 0 )
	{
		/* Maximum number of channels a user can join. Values <= 0 are equal to "no limit". */
#ifdef HAVE_ISDIGIT
		if( ! isdigit( (int)*Arg )) Config_Error_NaN( Line, Var );
		else
#endif
		Conf_MaxJoins = atoi( Arg );
		return;
	}
	if( strcasecmp( Var, "Listen" ) == 0 )
	{
		/* IP-Address to bind sockets */
		if( strlcpy( Conf_ListenAddress, Arg, sizeof( Conf_ListenAddress )) >= sizeof( Conf_ListenAddress ))
		{
			Config_Error_TooLong( Line, Var );
		}
		return;
	}

	Config_Error( LOG_ERR, "%s, line %d (section \"Global\"): Unknown variable \"%s\"!", NGIRCd_ConfFile, Line, Var );
} /* Handle_GLOBAL */


LOCAL void
Handle_OPERATOR( int Line, char *Var, char *Arg )
{
	assert( Line > 0 );
	assert( Var != NULL );
	assert( Arg != NULL );
	assert( Conf_Oper_Count > 0 );

	if( strcasecmp( Var, "Name" ) == 0 )
	{
		/* Name of IRC operator */
		if( strlcpy( Conf_Oper[Conf_Oper_Count - 1].name, Arg, sizeof( Conf_Oper[Conf_Oper_Count - 1].name )) >= sizeof( Conf_Oper[Conf_Oper_Count - 1].name )) Config_Error_TooLong( Line, Var );
		return;
	}
	if( strcasecmp( Var, "Password" ) == 0 )
	{
		/* Password of IRC operator */
		if( strlcpy( Conf_Oper[Conf_Oper_Count - 1].pwd, Arg, sizeof( Conf_Oper[Conf_Oper_Count - 1].pwd )) >= sizeof( Conf_Oper[Conf_Oper_Count - 1].pwd )) Config_Error_TooLong( Line, Var );
		return;
	}
	if( strcasecmp( Var, "Mask" ) == 0 )
	{
		if (Conf_Oper[Conf_Oper_Count - 1].mask) return; /* Hostname already configured */
		Conf_Oper[Conf_Oper_Count - 1].mask = strdup( Arg );
		if (! Conf_Oper[Conf_Oper_Count - 1].mask) {
			Config_Error( LOG_ERR, "%s, line %d: Cannot allocate memory for operator mask: %s", NGIRCd_ConfFile, Line, strerror(errno) );
			return;
		}

		return;
	}
	Config_Error( LOG_ERR, "%s, line %d (section \"Operator\"): Unknown variable \"%s\"!", NGIRCd_ConfFile, Line, Var );
} /* Handle_OPERATOR */


LOCAL void
Handle_SERVER( int Line, char *Var, char *Arg )
{
	long port;
	
	assert( Line > 0 );
	assert( Var != NULL );
	assert( Arg != NULL );

	/* Ignore server block if no space is left in server configuration structure */
	if( New_Server_Idx <= NONE ) return;

	if( strcasecmp( Var, "Host" ) == 0 )
	{
		/* Hostname of the server */
		if( strlcpy( New_Server.host, Arg, sizeof( New_Server.host )) >= sizeof( New_Server.host ))
			Config_Error_TooLong ( Line, Var );

		return;
	}
	if( strcasecmp( Var, "Name" ) == 0 )
	{
		/* Name of the server ("Nick"/"ID") */
		if( strlcpy( New_Server.name, Arg, sizeof( New_Server.name )) >= sizeof( New_Server.name ))
			Config_Error_TooLong( Line, Var );
		return;
	}
	if( strcasecmp( Var, "MyPassword" ) == 0 )
	{
		/* Password of this server which is sent to the peer */
		if( strlcpy( New_Server.pwd_in, Arg, sizeof( New_Server.pwd_in )) >= sizeof( New_Server.pwd_in )) Config_Error_TooLong( Line, Var );
		return;
	}
	if( strcasecmp( Var, "PeerPassword" ) == 0 )
	{
		/* Passwort of the peer which must be received */
		if( strlcpy( New_Server.pwd_out, Arg, sizeof( New_Server.pwd_out )) >= sizeof( New_Server.pwd_out )) Config_Error_TooLong( Line, Var );
		return;
	}
	if( strcasecmp( Var, "Port" ) == 0 )
	{
		/* Port to which this server should connect */
		port = atol( Arg );
		if( port > 0 && port < 0xFFFF ) New_Server.port = (UINT16)port;
		else Config_Error( LOG_ERR, "%s, line %d (section \"Server\"): Illegal port number %ld!", NGIRCd_ConfFile, Line, port );
		return;
	}
	if( strcasecmp( Var, "Group" ) == 0 )
	{
		/* Server group */
#ifdef HAVE_ISDIGIT
		if( ! isdigit( (int)*Arg )) Config_Error_NaN( Line, Var );
		else
#endif
		New_Server.group = atoi( Arg );
		return;
	}
	
	Config_Error( LOG_ERR, "%s, line %d (section \"Server\"): Unknown variable \"%s\"!", NGIRCd_ConfFile, Line, Var );
} /* Handle_SERVER */


LOCAL void
Handle_CHANNEL( int Line, char *Var, char *Arg )
{
	assert( Line > 0 );
	assert( Var != NULL );
	assert( Arg != NULL );

	if( strcasecmp( Var, "Name" ) == 0 )
	{
		/* Name of the channel */
		if( strlcpy( Conf_Channel[Conf_Channel_Count - 1].name, Arg, sizeof( Conf_Channel[Conf_Channel_Count - 1].name )) >= sizeof( Conf_Channel[Conf_Channel_Count - 1].name ))
			Config_Error_TooLong( Line, Var );
		return;
	}
	if( strcasecmp( Var, "Modes" ) == 0 )
	{
		/* Initial modes */
		if( strlcpy( Conf_Channel[Conf_Channel_Count - 1].modes, Arg, sizeof( Conf_Channel[Conf_Channel_Count - 1].modes )) >= sizeof( Conf_Channel[Conf_Channel_Count - 1].modes ))
			Config_Error_TooLong( Line, Var );
		return;
	}
	if( strcasecmp( Var, "Topic" ) == 0 )
	{
		/* Initial topic */
		if( strlcpy( Conf_Channel[Conf_Channel_Count - 1].topic, Arg, sizeof( Conf_Channel[Conf_Channel_Count - 1].topic )) >= sizeof( Conf_Channel[Conf_Channel_Count - 1].topic ))
			Config_Error_TooLong( Line, Var );
 
		return;
	}

	Config_Error( LOG_ERR, "%s, line %d (section \"Channel\"): Unknown variable \"%s\"!", NGIRCd_ConfFile, Line, Var );
} /* Handle_CHANNEL */


LOCAL void
Validate_Config( bool Configtest )
{
	/* Validate configuration settings. */

#ifdef DEBUG
	int i, servers, servers_once;
#endif

	if( ! Conf_ServerName[0] )
	{
		/* No server name configured! */
		Config_Error( LOG_ALERT, "No server name configured in \"%s\" (section 'Global': 'Name')!", NGIRCd_ConfFile );
		if( ! Configtest )
		{
			Config_Error( LOG_ALERT, "%s exiting due to fatal errors!", PACKAGE_NAME );
			exit( 1 );
		}
	}
	
	if( Conf_ServerName[0] && ! strchr( Conf_ServerName, '.' ))
	{
		/* No dot in server name! */
		Config_Error( LOG_ALERT, "Invalid server name configured in \"%s\" (section 'Global': 'Name'): Dot missing!", NGIRCd_ConfFile );
		if( ! Configtest )
		{
			Config_Error( LOG_ALERT, "%s exiting due to fatal errors!", PACKAGE_NAME );
			exit( 1 );
		}
	}

#ifdef STRICT_RFC
	if( ! Conf_ServerAdminMail[0] )
	{
		/* No administrative contact configured! */
		Config_Error( LOG_ALERT, "No administrator email address configured in \"%s\" ('AdminEMail')!", NGIRCd_ConfFile );
		if( ! Configtest )
		{
			Config_Error( LOG_ALERT, "%s exiting due to fatal errors!", PACKAGE_NAME );
			exit( 1 );
		}
	}
#endif

	if( ! Conf_ServerAdmin1[0] && ! Conf_ServerAdmin2[0] && ! Conf_ServerAdminMail[0] )
	{
		/* No administrative information configured! */
		Config_Error( LOG_WARNING, "No administrative information configured but required by RFC!" );
	}
#ifdef FD_SETSIZE	
	if(( Conf_MaxConnections > (long)FD_SETSIZE ) || ( Conf_MaxConnections < 1 ))
	{
		Conf_MaxConnections = (long)FD_SETSIZE;
		Config_Error( LOG_ERR, "Setting MaxConnections to %ld, select() can't handle more file descriptors!", Conf_MaxConnections );
	}
#else
	Config_Error( LOG_WARN, "Don't know how many file descriptors select() can handle on this system, don't set MaxConnections too high!" );
#endif

#ifdef DEBUG
	servers = servers_once = 0;
	for( i = 0; i < MAX_SERVERS; i++ )
	{
		if( Conf_Server[i].name[0] )
		{
			servers++;
			if( Conf_Server[i].flags & CONF_SFLAG_ONCE ) servers_once++;
		}
	}
	Log( LOG_DEBUG, "Configuration: Operators=%d, Servers=%d[%d], Channels=%d", Conf_Oper_Count, servers, servers_once, Conf_Channel_Count );
#endif
} /* Validate_Config */


LOCAL void
Config_Error_TooLong ( const int Line, const char *Item )
{
	Config_Error( LOG_WARNING, "%s, line %d: Value of \"%s\" too long!", NGIRCd_ConfFile, Line, Item );
}

LOCAL void
Config_Error_NaN( const int Line, const char *Item )
{
	Config_Error( LOG_WARNING, "%s, line %d: Value of \"%s\" is not a number!", NGIRCd_ConfFile, Line, Item );
}

#ifdef PROTOTYPES
LOCAL void Config_Error( const int Level, const char *Format, ... )
#else
LOCAL void Config_Error( Level, Format, va_alist )
const int Level;
const char *Format;
va_dcl
#endif
{
	/* Error! Write to console and/or logfile. */

	char msg[MAX_LOG_MSG_LEN];
	va_list ap;

	assert( Format != NULL );

#ifdef PROTOTYPES
	va_start( ap, Format );
#else
	va_start( ap );
#endif
	vsnprintf( msg, MAX_LOG_MSG_LEN, Format, ap );
	va_end( ap );
	
	/* During "normal operations" the log functions of the daemon should
	 * be used, but during testing of the configuration file, all messages
	 * should go directly to the console: */
	if( Use_Log ) Log( Level, "%s", msg );
	else puts( msg );
} /* Config_Error */


LOCAL void
Init_Server_Struct( CONF_SERVER *Server )
{
	/* Initialize server configuration structur to default values */

	assert( Server != NULL );

	memset( Server, 0, sizeof (CONF_SERVER) );

	Server->group = NONE;
	Server->lasttry = time( NULL ) - Conf_ConnectRetry + STARTUP_DELAY;

	if( NGIRCd_Passive ) Server->flags = CONF_SFLAG_DISABLED;

	Server->conn_id = NONE;
} /* Init_Server_Struct */


/* -eof- */
