#ifndef __GST_KURENTO_PLUGINS_CONFIG_H__
#define __GST_KURENTO_PLUGINS_CONFIG_H__

/* Version */
#cmakedefine VERSION "@VERSION@"

/* Package name*/
#cmakedefine PACKAGE "@PACKAGE@"

/* The gettext domain name */
#cmakedefine GETTEXT_PACKAGE "@GETTEXT_PACKAGE@"

/* Shared files install */
#cmakedefine DATAROOTDIR "@DATAROOTDIR@"

/* Binary files directory */
#cmakedefine BINARY_LOCATION "@BINARY_LOCATION@"

/* Tests will generate files for manual check if this macro is defined */
#cmakedefine MANUAL_CHECK

#endif /* __GST_KURENTO_PLUGINS_CONFIG_H__ */
