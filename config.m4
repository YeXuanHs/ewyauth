PHP_ARG_ENABLE(ewyauth, whether to enable ewyauth support,
[  --enable-ewyauth        Enable ewyauth support])

if test "$PHP_EWYAUTH" != "no"; then
  PHP_NEW_EXTENSION(ewyauth, ewyauth.c, $ext_shared)
fi
