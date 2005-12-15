/*
 * ngIRCd -- The Next Generation IRC Daemon
 * Copyright (c)2001,2002 by Alexander Barton (alex@barton.de)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * Please read the file COPYING, README and AUTHORS for more information.
 *
 * Login and logout
 */


#include "portab.h"

static char UNUSED id[] = "$Id: irc-login.c,v 1.44.2.1 2005/12/15 11:01:59 alex Exp $";

#include "imp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ngircd.h"
#include "resolve.h"
#include "conn-func.h"
#include "conf.h"
#include "client.h"
#include "channel.h"
#include "log.h"
#include "messages.h"
#include "parse.h"
#include "irc.h"
#include "irc-info.h"
#include "irc-write.h"
#include "cvs-version.h"

#include "exp.h"
#include "irc-login.h"


LOCAL bool Hello_User PARAMS(( CLIENT *Client ));
LOCAL void Kill_Nick PARAMS(( char *Nick, char *Reason ));


GLOBAL bool
IRC_PASS( CLIENT *Client, REQUEST *Req )
{
	assert( Client != NULL );
	assert( Req != NULL );

	/* Fehler liefern, wenn kein lokaler Client */
	if( Client_Conn( Client ) <= NONE ) return IRC_WriteStrClient( Client, ERR_UNKNOWNCOMMAND_MSG, Client_ID( Client ), Req->command );
	
	if(( Client_Type( Client ) == CLIENT_UNKNOWN ) && ( Req->argc == 1))
	{
		/* noch nicht registrierte unbekannte Verbindung */
		Log( LOG_DEBUG, "Connection %d: got PASS command ...", Client_Conn( Client ));

		/* Passwort speichern */
		Client_SetPassword( Client, Req->argv[0] );

		Client_SetType( Client, CLIENT_GOTPASS );
		return CONNECTED;
	}
	else if((( Client_Type( Client ) == CLIENT_UNKNOWN ) || ( Client_Type( Client ) == CLIENT_UNKNOWNSERVER )) && (( Req->argc == 3 ) || ( Req->argc == 4 )))
	{
		char c2, c4, *type, *impl, *serverver, *flags, *ptr, *ircflags;
		int protohigh, protolow;

		/* noch nicht registrierte Server-Verbindung */
		Log( LOG_DEBUG, "Connection %d: got PASS command (new server link) ...", Client_Conn( Client ));

		/* Passwort speichern */
		Client_SetPassword( Client, Req->argv[0] );

		/* Protokollversion ermitteln */
		if( strlen( Req->argv[1] ) >= 4 )
		{
			c2 = Req->argv[1][2];
			c4 = Req->argv[1][4];

			Req->argv[1][4] = '\0';
			protolow = atoi( &Req->argv[1][2] );
			Req->argv[1][2] = '\0';
			protohigh = atoi( Req->argv[1] );
			
			Req->argv[1][2] = c2;
			Req->argv[1][4] = c4;
		}			
		else protohigh = protolow = 0;

		/* Protokoll-Typ */
		if( strlen( Req->argv[1] ) > 4 ) type = &Req->argv[1][4];
		else type = NULL;

		/* IRC-Flags (nach RFC 2813) */
		if( Req->argc >= 4 ) ircflags = Req->argv[3];
		else ircflags = "";

		/* Implementation, Version und ngIRCd-Flags */
		impl = Req->argv[2];
		ptr = strchr( impl, '|' );
		if( ptr ) *ptr = '\0';

		if( type && ( strcmp( type, PROTOIRCPLUS ) == 0 ))
		{
			/* auf der anderen Seite laeuft ein Server, der
			 * ebenfalls das IRC+-Protokoll versteht */
			serverver = ptr + 1;
			flags = strchr( serverver, ':' );
			if( flags )
			{
				*flags = '\0';
				flags++;
			}
			else flags = "";
			Log( LOG_INFO, "Peer announces itself as %s-%s using protocol %d.%d/IRC+ (flags: \"%s\").", impl, serverver, protohigh, protolow, flags );
		}
		else
		{
			/* auf der anderen Seite laeuft ein Server, der
			 * nur das Originalprotokoll unterstuetzt */
			serverver = "";
			if( strchr( ircflags, 'Z' )) flags = "Z";
			else flags = "";
			Log( LOG_INFO, "Peer announces itself as \"%s\" using protocol %d.%d (flags: \"%s\").", impl, protohigh, protolow, flags );
		}

		Client_SetType( Client, CLIENT_GOTPASSSERVER );
		Client_SetFlags( Client, flags );

		return CONNECTED;
	}
	else if(( Client_Type( Client ) == CLIENT_UNKNOWN  ) || ( Client_Type( Client ) == CLIENT_UNKNOWNSERVER ))
	{
		/* Falsche Anzahl Parameter? */
		return IRC_WriteStrClient( Client, ERR_NEEDMOREPARAMS_MSG, Client_ID( Client ), Req->command );
	}
	else return IRC_WriteStrClient( Client, ERR_ALREADYREGISTRED_MSG, Client_ID( Client ));
} /* IRC_PASS */


/**
 * IRC "NICK" command.
 * This function implements the IRC command "NICK" which is used to register
 * with the server, to change already registered nicknames and to introduce
 * new users which are connected to other servers.
 */
GLOBAL bool
IRC_NICK( CLIENT *Client, REQUEST *Req )
{
	CLIENT *intr_c, *target, *c;
	char *modes;

	assert( Client != NULL );
	assert( Req != NULL );

#ifndef STRICT_RFC
	/* Some IRC clients, for example BitchX, send the NICK and USER
	 * commands in the wrong order ... */
	if( Client_Type( Client ) == CLIENT_UNKNOWN
	    || Client_Type( Client ) == CLIENT_GOTPASS
	    || Client_Type( Client ) == CLIENT_GOTNICK
	    || Client_Type( Client ) == CLIENT_GOTUSER
	    || Client_Type( Client ) == CLIENT_USER
	    || ( Client_Type( Client ) == CLIENT_SERVER && Req->argc == 1 ))
#else
	if( Client_Type( Client ) == CLIENT_UNKNOWN
	    || Client_Type( Client ) == CLIENT_GOTPASS
	    || Client_Type( Client ) == CLIENT_GOTNICK
	    || Client_Type( Client ) == CLIENT_USER
	    || ( Client_Type( Client ) == CLIENT_SERVER && Req->argc == 1 ))
#endif
	{
		/* User registration or change of nickname */

		/* Wrong number of arguments? */
		if( Req->argc != 1 )
			return IRC_WriteStrClient( Client, ERR_NEEDMOREPARAMS_MSG,
						   Client_ID( Client ),
						   Req->command );

		/* Search "target" client */
		if( Client_Type( Client ) == CLIENT_SERVER )
		{
			target = Client_Search( Req->prefix );
			if( ! target )
				return IRC_WriteStrClient( Client,
							   ERR_NOSUCHNICK_MSG,
							   Client_ID( Client ),
							   Req->argv[0] );
		}
		else
		{
			/* Is this a restricted client? */
			if( Client_HasMode( Client, 'r' ))
				return IRC_WriteStrClient( Client,
							   ERR_RESTRICTED_MSG,
							   Client_ID( Client ));

			target = Client;
		}

#ifndef STRICT_RFC
		/* If the clients tries to change to its own nickname we won't
		 * do anything. This is how the original ircd behaves and some
		 * clients (for example Snak) expect it to be like this.
		 * But I doubt that this is "really the right thing" ... */
		if( strcmp( Client_ID( target ), Req->argv[0] ) == 0 )
			return CONNECTED;
#endif

		/* Check that the new nickname is available. Special case:
		 * the client only changes from/to upper to lower case. */
		if( strcasecmp( Client_ID( target ), Req->argv[0] ) != 0 )
		{
			if( ! Client_CheckNick( target, Req->argv[0] ))
				return CONNECTED;
		}

		if(( Client_Type( target ) != CLIENT_USER )
		   && ( Client_Type( target ) != CLIENT_SERVER ))
		{
			/* New client */
			Log( LOG_DEBUG, "Connection %d: got valid NICK command ...", 
			     Client_Conn( Client ));

			/* Register new nickname of this client */
			Client_SetID( target, Req->argv[0] );

			/* If we received a valid USER command already then
			 * register the new client! */
			if( Client_Type( Client ) == CLIENT_GOTUSER )
				return Hello_User( Client );
			else
				Client_SetType( Client, CLIENT_GOTNICK );
		}
		else
		{
			/* Nickname change */
			if( Client_Conn( target ) > NONE )
			{
				/* Local client */
				Log( LOG_INFO,
				     "User \"%s\" changed nick (connection %d): \"%s\" -> \"%s\".",
				     Client_Mask( target ), Client_Conn( target ),
				     Client_ID( target ), Req->argv[0] );
			}
			else
			{
				/* Remote client */
				Log( LOG_DEBUG,
				     "User \"%s\" changed nick: \"%s\" -> \"%s\".",
				     Client_Mask( target ), Client_ID( target ),
				     Req->argv[0] );
			}

			/* Inform all users and servers (which have to know)
			 * of this nickname change */
			if( Client_Type( Client ) == CLIENT_USER )
				IRC_WriteStrClientPrefix( Client, Client,
							  "NICK :%s",
							  Req->argv[0] );
			IRC_WriteStrServersPrefix( Client, target,
						   "NICK :%s", Req->argv[0] );
			IRC_WriteStrRelatedPrefix( target, target, false,
						   "NICK :%s", Req->argv[0] );
			
			/* Register old nickname for WHOWAS queries */
			Client_RegisterWhowas( target );
				
			/* Save new nickname */
			Client_SetID( target, Req->argv[0] );
			
			IRC_SetPenalty( target, 2 );
		}

		return CONNECTED;
	}
	else if( Client_Type( Client ) == CLIENT_SERVER )
	{
		/* Server introduces new client */

		/* Falsche Anzahl Parameter? */
		if( Req->argc != 7 ) return IRC_WriteStrClient( Client, ERR_NEEDMOREPARAMS_MSG, Client_ID( Client ), Req->command );

		/* Nick ueberpruefen */
		c = Client_Search( Req->argv[0] );
		if( c )
		{
			/* Der neue Nick ist auf diesem Server bereits registriert:
			 * sowohl der neue, als auch der alte Client muessen nun
			 * disconnectiert werden. */
			Log( LOG_ERR, "Server %s introduces already registered nick \"%s\"!", Client_ID( Client ), Req->argv[0] );
			Kill_Nick( Req->argv[0], "Nick collision" );
			return CONNECTED;
		}

		/* Server, zu dem der Client connectiert ist, suchen */
		intr_c = Client_GetFromToken( Client, atoi( Req->argv[4] ));
		if( ! intr_c )
		{
			Log( LOG_ERR, "Server %s introduces nick \"%s\" on unknown server!?", Client_ID( Client ), Req->argv[0] );
			Kill_Nick( Req->argv[0], "Unknown server" );
			return CONNECTED;
		}

		/* Neue Client-Struktur anlegen */
		c = Client_NewRemoteUser( intr_c, Req->argv[0], atoi( Req->argv[1] ), Req->argv[2], Req->argv[3], atoi( Req->argv[4] ), Req->argv[5] + 1, Req->argv[6], true);
		if( ! c )
		{
			/* Eine neue Client-Struktur konnte nicht angelegt werden.
			 * Der Client muss disconnectiert werden, damit der Netz-
			 * status konsistent bleibt. */
			Log( LOG_ALERT, "Can't create client structure! (on connection %d)", Client_Conn( Client ));
			Kill_Nick( Req->argv[0], "Server error" );
			return CONNECTED;
		}

		modes = Client_Modes( c );
		if( *modes ) Log( LOG_DEBUG, "User \"%s\" (+%s) registered (via %s, on %s, %d hop%s).", Client_Mask( c ), modes, Client_ID( Client ), Client_ID( intr_c ), Client_Hops( c ), Client_Hops( c ) > 1 ? "s": "" );
		else Log( LOG_DEBUG, "User \"%s\" registered (via %s, on %s, %d hop%s).", Client_Mask( c ), Client_ID( Client ), Client_ID( intr_c ), Client_Hops( c ), Client_Hops( c ) > 1 ? "s": "" );

		/* Andere Server, ausser dem Introducer, informieren */
		IRC_WriteStrServersPrefix( Client, Client, "NICK %s %d %s %s %d %s :%s", Req->argv[0], atoi( Req->argv[1] ) + 1, Req->argv[2], Req->argv[3], Client_MyToken( intr_c ), Req->argv[5], Req->argv[6] );

		return CONNECTED;
	}
	else return IRC_WriteStrClient( Client, ERR_ALREADYREGISTRED_MSG, Client_ID( Client ));
} /* IRC_NICK */


GLOBAL bool
IRC_USER( CLIENT *Client, REQUEST *Req )
{
#ifdef IDENTAUTH
	char *ptr;
#endif

	assert( Client != NULL );
	assert( Req != NULL );

#ifndef STRICT_RFC
	if( Client_Type( Client ) == CLIENT_GOTNICK || Client_Type( Client ) == CLIENT_GOTPASS || Client_Type( Client ) == CLIENT_UNKNOWN )
#else
	if( Client_Type( Client ) == CLIENT_GOTNICK || Client_Type( Client ) == CLIENT_GOTPASS )
#endif
	{
		/* Wrong number of parameters? */
		if( Req->argc != 4 ) return IRC_WriteStrClient( Client, ERR_NEEDMOREPARAMS_MSG, Client_ID( Client ), Req->command );

		/* User name */
#ifdef IDENTAUTH
		ptr = Client_User( Client );
		if( ! ptr || ! *ptr || *ptr == '~' ) Client_SetUser( Client, Req->argv[0], false );
#else
		Client_SetUser( Client, Req->argv[0], false );
#endif

		/* "Real name" or user info text: Don't set it to the empty string, the original ircd
		 * can't deal with such "real names" (e. g. "USER user * * :") ... */
		if( *Req->argv[3] ) Client_SetInfo( Client, Req->argv[3] );
		else Client_SetInfo( Client, "-" );

		Log( LOG_DEBUG, "Connection %d: got valid USER command ...", Client_Conn( Client ));
		if( Client_Type( Client ) == CLIENT_GOTNICK ) return Hello_User( Client );
		else Client_SetType( Client, CLIENT_GOTUSER );
		return CONNECTED;
	}
	else if( Client_Type( Client ) == CLIENT_USER || Client_Type( Client ) == CLIENT_SERVER || Client_Type( Client ) == CLIENT_SERVICE )
	{
		return IRC_WriteStrClient( Client, ERR_ALREADYREGISTRED_MSG, Client_ID( Client ));
	}
	else return IRC_WriteStrClient( Client, ERR_NOTREGISTERED_MSG, Client_ID( Client ));
} /* IRC_USER */


GLOBAL bool
IRC_QUIT( CLIENT *Client, REQUEST *Req )
{
	CLIENT *target;
	char quitmsg[LINE_LEN];
	
	assert( Client != NULL );
	assert( Req != NULL );
		
	/* Wrong number of arguments? */
	if( Req->argc > 1 )
		return IRC_WriteStrClient( Client, ERR_NEEDMOREPARAMS_MSG, Client_ID( Client ), Req->command );

	if (Req->argc == 1)
		strlcpy(quitmsg, Req->argv[0], sizeof quitmsg);

	if ( Client_Type( Client ) == CLIENT_SERVER )
	{
		/* Server */
		target = Client_Search( Req->prefix );
		if( ! target )
		{
			/* Den Client kennen wir nicht (mehr), also nichts zu tun. */
			Log( LOG_WARNING, "Got QUIT from %s for unknown client!?", Client_ID( Client ));
			return CONNECTED;
		}

		Client_Destroy( target, "Got QUIT command.", Req->argc == 1 ? quitmsg : NULL, true);

		return CONNECTED;
	}
	else
	{
		if (Req->argc == 1 && quitmsg[0] != '\"') {
			/* " " to avoid confusion */
			strlcpy(quitmsg, "\"", sizeof quitmsg);
			strlcat(quitmsg, Req->argv[0], sizeof quitmsg-1);
			strlcat(quitmsg, "\"", sizeof quitmsg );
		}

		/* User, Service, oder noch nicht registriert */
		Conn_Close( Client_Conn( Client ), "Got QUIT command.", Req->argc == 1 ? quitmsg : NULL, true);
		
		return DISCONNECTED;
	}
} /* IRC_QUIT */


GLOBAL bool
IRC_PING( CLIENT *Client, REQUEST *Req )
{
	CLIENT *target, *from;

	assert( Client != NULL );
	assert( Req != NULL );

	/* Falsche Anzahl Parameter? */
	if( Req->argc < 1 ) return IRC_WriteStrClient( Client, ERR_NOORIGIN_MSG, Client_ID( Client ));
#ifdef STRICT_RFC
	if( Req->argc > 2 ) return IRC_WriteStrClient( Client, ERR_NEEDMOREPARAMS_MSG, Client_ID( Client ), Req->command );
#endif

	if( Req->argc > 1 )
	{
		/* es wurde ein Ziel-Client angegeben */
		target = Client_Search( Req->argv[1] );
		if(( ! target ) || ( Client_Type( target ) != CLIENT_SERVER )) return IRC_WriteStrClient( Client, ERR_NOSUCHSERVER_MSG, Client_ID( Client ), Req->argv[1] );
		if( target != Client_ThisServer( ))
		{
			/* ok, forwarden */
			if( Client_Type( Client ) == CLIENT_SERVER ) from = Client_Search( Req->prefix );
			else from = Client;
			if( ! from ) return IRC_WriteStrClient( Client, ERR_NOSUCHSERVER_MSG, Client_ID( Client ), Req->prefix );
			return IRC_WriteStrClientPrefix( target, from, "PING %s :%s", Client_ID( from ), Req->argv[1] );
		}
	}

	Log( LOG_DEBUG, "Connection %d: got PING, sending PONG ...", Client_Conn( Client ));
#ifdef STRICT_RFC
	return IRC_WriteStrClient(Client, "PONG %s :%s",
		Client_ID(Client_ThisServer()), Client_ID(Client));
#else
	/* Some clients depend on the argument being returned in the PONG
	 * reply (not mentioned in any RFC, though) */
	return IRC_WriteStrClient(Client, "PONG %s :%s",
		Client_ID(Client_ThisServer( )), Req->argv[0]);
#endif	
} /* IRC_PING */


GLOBAL bool
IRC_PONG( CLIENT *Client, REQUEST *Req )
{
	CLIENT *target, *from;

	assert( Client != NULL );
	assert( Req != NULL );

	/* Falsche Anzahl Parameter? */
	if( Req->argc < 1 ) return IRC_WriteStrClient( Client, ERR_NOORIGIN_MSG, Client_ID( Client ));
	if( Req->argc > 2 ) return IRC_WriteStrClient( Client, ERR_NEEDMOREPARAMS_MSG, Client_ID( Client ), Req->command );

	/* forwarden? */
	if( Req->argc == 2 )
	{
		target = Client_Search( Req->argv[1] );
		if(( ! target ) || ( Client_Type( target ) != CLIENT_SERVER )) return IRC_WriteStrClient( Client, ERR_NOSUCHSERVER_MSG, Client_ID( Client ), Req->argv[1] );
		if( target != Client_ThisServer( ))
		{
			/* ok, forwarden */
			if( Client_Type( Client ) == CLIENT_SERVER ) from = Client_Search( Req->prefix );
			else from = Client;
			if( ! from ) return IRC_WriteStrClient( Client, ERR_NOSUCHSERVER_MSG, Client_ID( Client ), Req->prefix );
			return IRC_WriteStrClientPrefix( target, from, "PONG %s :%s", Client_ID( from ), Req->argv[1] );
		}
	}

	/* Der Connection-Timestamp wurde schon beim Lesen aus dem Socket
	 * aktualisiert, daher muss das hier nicht mehr gemacht werden. */

	if( Client_Conn( Client ) > NONE ) Log( LOG_DEBUG, "Connection %d: received PONG. Lag: %ld seconds.", Client_Conn( Client ), time( NULL ) - Conn_LastPing( Client_Conn( Client )));
	else Log( LOG_DEBUG, "Connection %d: received PONG.", Client_Conn( Client ));

	return CONNECTED;
} /* IRC_PONG */


LOCAL bool
Hello_User( CLIENT *Client )
{
#ifdef CVSDATE
	char ver[12], vertxt[30];
#endif

	assert( Client != NULL );

	/* Check password ... */
	if( strcmp( Client_Password( Client ), Conf_ServerPwd ) != 0 )
	{
		/* Bad password! */
		Log( LOG_ERR, "User \"%s\" rejected (connection %d): Bad password!", Client_Mask( Client ), Client_Conn( Client ));
		Conn_Close( Client_Conn( Client ), NULL, "Bad password", true);
		return DISCONNECTED;
	}

	Log( LOG_NOTICE, "User \"%s\" registered (connection %d).", Client_Mask( Client ), Client_Conn( Client ));

	/* Inform other servers */
	IRC_WriteStrServers( NULL, "NICK %s 1 %s %s 1 +%s :%s", Client_ID( Client ), Client_User( Client ), Client_Hostname( Client ), Client_Modes( Client ), Client_Info( Client ));

	/* Welcome :-) */
	if( ! IRC_WriteStrClient( Client, RPL_WELCOME_MSG, Client_ID( Client ), Client_Mask( Client ))) return false;

	/* Version and system type */
#ifdef CVSDATE
	strlcpy( ver, CVSDATE, sizeof( ver ));
	strncpy( ver + 4, ver + 5, 2 );
	strncpy( ver + 6, ver + 8, 3 );
	snprintf( vertxt, sizeof( vertxt ), "%s(%s)", PACKAGE_VERSION, ver );
	if( ! IRC_WriteStrClient( Client, RPL_YOURHOST_MSG, Client_ID( Client ), Client_ID( Client_ThisServer( )), vertxt, TARGET_CPU, TARGET_VENDOR, TARGET_OS )) return false;
#else
	if( ! IRC_WriteStrClient( Client, RPL_YOURHOST_MSG, Client_ID( Client ), Client_ID( Client_ThisServer( )), PACKAGE_VERSION, TARGET_CPU, TARGET_VENDOR, TARGET_OS )) return false;
#endif

	if( ! IRC_WriteStrClient( Client, RPL_CREATED_MSG, Client_ID( Client ), NGIRCd_StartStr )) return false;
#ifdef CVSDATE
	if( ! IRC_WriteStrClient( Client, RPL_MYINFO_MSG, Client_ID( Client ), Client_ID( Client_ThisServer( )), vertxt, USERMODES, CHANMODES )) return false;	
#else
	if( ! IRC_WriteStrClient( Client, RPL_MYINFO_MSG, Client_ID( Client ), Client_ID( Client_ThisServer( )), PACKAGE_VERSION, USERMODES, CHANMODES )) return false;
#endif

	/* Features */
	if( ! IRC_WriteStrClient( Client, RPL_ISUPPORT_MSG, Client_ID( Client ), CLIENT_NICK_LEN - 1, CHANNEL_TOPIC_LEN - 1, CLIENT_AWAY_LEN - 1, Conf_MaxJoins )) return DISCONNECTED;

	Client_SetType( Client, CLIENT_USER );

	if( ! IRC_Send_LUSERS( Client )) return DISCONNECTED;
	if( ! IRC_Show_MOTD( Client )) return DISCONNECTED;

	/* Suspend the client for a second ... */
	IRC_SetPenalty( Client, 1 );

	return CONNECTED;
} /* Hello_User */


LOCAL void
Kill_Nick( char *Nick, char *Reason )
{
	REQUEST r;

	assert( Nick != NULL );
	assert( Reason != NULL );

	r.prefix = (char *)Client_ThisServer( );
	r.argv[0] = Nick;
	r.argv[1] = Reason;
	r.argc = 2;

	Log( LOG_ERR, "User(s) with nick \"%s\" will be disconnected: %s", Nick, Reason );
	IRC_KILL( Client_ThisServer( ), &r );
} /* Kill_Nick */


/* -eof- */
