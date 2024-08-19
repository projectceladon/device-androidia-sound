/*#define LOG_NDEBUG 0*/

//#define DEBUG_PCM_DUMP

#ifdef DEBUG_PCM_DUMP
// To enable dumps, explicitly create "/vendor/dump/" folder and reboot device
FILE *sco_call_write = NULL;
FILE *sco_call_write_remapped = NULL;
FILE *sco_call_write_bt = NULL;
FILE *sco_call_read = NULL;
FILE *sco_call_read_remapped = NULL;
FILE *sco_call_read_bt = NULL;
FILE *out_write_dump = NULL;
FILE *in_read_dump = NULL;
#endif

