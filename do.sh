rm -rf a.out
gcc main.c   src/log.c  -I./src -lpthread  -g  -DLOG_USE_COLOR

