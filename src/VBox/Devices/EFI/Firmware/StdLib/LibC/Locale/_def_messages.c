/*  $NetBSD: _def_messages.c,v 1.6 2005/06/12 05:21:27 lukem Exp $  */

/*
 * Written by J.T. Conklin <jtc@NetBSD.org>.
 * Public domain.
 */
#include  <LibConfig.h>
#include  <sys/EfiCdefs.h>
#if defined(LIBC_SCCS) && !defined(lint)
__RCSID("$NetBSD: _def_messages.c,v 1.6 2005/06/12 05:21:27 lukem Exp $");
#endif /* LIBC_SCCS and not lint */

#include <sys/localedef.h>
#include <locale.h>

const _MessagesLocale _DefaultMessagesLocale =
{
  "^[Yy]",
  "^[Nn]",
  "yes",
  "no"
} ;

const _MessagesLocale *_CurrentMessagesLocale = &_DefaultMessagesLocale;
