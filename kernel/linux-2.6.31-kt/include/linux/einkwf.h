/*
 *  linux/include/linux/einkwf.h -- eInk waveform parsing definitions
 *
 *      Copyright (c) 2011 Amazon Technologies, Inc.
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License. See the file COPYING in the main directory of this archive for
 *  more details.
 */

#ifndef _EINKWF_H
#define _EINKWF_H

#include <linux/types.h>

struct eink_waveform_t
{
    unsigned char   version,			// EINK_ADDR_WAVEFORM_VERSION
                    subversion,			// EINK_ADDR_WAVEFORM_SUBVERSION
                    type,			// EINK_ADDR_WAVEFORM_TYPE
                    run_type,			// EINK_ADDR_RUN_TYPE
                    mode_version,		// EINK_ADDR_MODE_VERSION
                    mfg_code,			// EINK_ADDR_MFG_CODE
                    tuning_bias,		// EINK_ADDR_WAVEFORM_TUNING_BIAS
                    revision,			// EINK_ADDR_WAVEFORM_REV (WR-spec only)
		    fpl_rate,			// EINK_ADDR_FPL_RATE
                    vcom_shift;			// EINK_ADDR_VCOM_SHIFT
    unsigned long   serial_number;		// EINK_ADDR_SERIAL_NUMBER
    unsigned long   xwia;			// EINK_ADDR_XWIA

    bool            parse_wf_hex;
};
typedef struct eink_waveform_t eink_waveform_t;

struct eink_fpl_t
{
    unsigned char   platform,			// EINK_ADDR_FPL_PLATFORM
                    size,			// EINK_ADDR_FPL_SIZE
                    adhesive_run_number;	// EINK_ADDR_ADHESIVE_RUN_NUM

    unsigned short  lot;			// EINK_ADDR_FPL_LOT
};
typedef struct eink_fpl_t eink_fpl_t;

struct eink_waveform_info_t
{
    struct eink_waveform_t waveform;
    struct eink_fpl_t fpl;

    unsigned long   filesize,                   // EINK_ADDR_FILESIZE
                    checksum;                   // EINK_ADDR_FILESIZE ? EINK_ADDR_CHECKSUM : (EINK_ADDR_CHECKSUM2 << 16) | EINK_ADDR_CHECKSUM1
};
typedef struct eink_waveform_info_t eink_waveform_info_t;

enum eink_waveform_version_string_t
{
    eink_waveform_version_string,
    eink_waveform_filename
};
typedef enum eink_waveform_version_string_t eink_waveform_version_string_t;

struct eink_commands_info_t
{
    int             which;              // EINK_COMMANDS_BROADSHEET || EINK_COMMANDS_ISIS
    
    unsigned char   vers_major,         // Broadsheet
                    vers_minor;         //
    unsigned short  type;               //

    unsigned long   version;            // ISIS
    
    unsigned long   checksum;           // Broadsheet = CRC32, ISIS = SUM32
};
typedef struct eink_commands_info_t eink_commands_info_t;

#define EINK_WF_UNKNOWN_PATH    "no path set yet"
#define EINK_WF_DEFAULT_USAGE   "default waveform usage"
#define EINK_WF_USE_BUILTIN_WAVEFORM "built-in"

#define EINK_WF_WBF_EXTENSION   ".wbf"
#define EINK_WF_RAW_EXTENSION   ".wrf"

// Canonical Waveform Update Modes.
//
#define WF_UPD_MODE_INIT         0      //  INIT (panel initialization)
#define WF_UPD_MODE_MU           1      //  MU   (monochrome update)
#define WF_UPD_MODE_GU           2      //  GU   (grayscale update)
#define WF_UPD_MODE_GC           2      //  GC   (grayscale clear)
#define WF_UPD_MODE_GCF          3      //  GCF  (text to text)
#define WF_UPD_MODE_PU           4      //  PU   (pen update)
#define WF_UPD_MODE_GL           5      //  GL   (white transition update)
#define WF_UPD_MODE_GLF          6      //  GLF  (text to text)
#define WF_UPD_MODE_DU4          7      //  DU4  (text to text)

#define WF_UPD_MODE_INVALID      (-1)
#define WF_UPD_MODE_AUTO         WAVEFORM_MODE_AUTO

void einkwf_set_buffer_size(int size);
int  einkwf_get_buffer_size(void);

void einkwf_set_buffer(char *buffer);
char *einkwf_get_buffer(void);

void einkwf_get_waveform_info(eink_waveform_info_t *info);
void einkwf_get_waveform_version(eink_waveform_t *waveform);
void einkwf_get_fpl_version(eink_fpl_t *fpl);
char *einkwf_get_waveform_version_string(eink_waveform_version_string_t which_string);
bool einkwf_waveform_valid(void);

int  einkwf_write_waveform_to_file(char *waveform_file_path);
int  einkwf_read_waveform_from_file(char *waveform_file_path);

bool einkwf_get_wavform_proxy_from_waveform(char *waveform_file_path, char *waveform_proxy_path);

void einkwf_get_commands_info(eink_commands_info_t *info);
char *einkwf_get_commands_version_string(void);
bool einkwf_commands_valid(void);

bool einkwf_panel_flash_present(void);
u8 *einkwf_panel_get_waveform(char *waveform_file_path, int *waveform_proxy_size);
void einkwf_panel_get_waveform_info(eink_waveform_info_t *info);
char *einkwf_panel_get_waveform_version_string(eink_waveform_version_string_t which_string);
void einkwf_panel_waveform_free(u8 *waveform_proxy);
int einkwf_panel_get_waveform_mode(int upd_mode);
void einkwf_panel_set_update_modes(void);

bool einkwf_panel_supports_vcom(void);
int einkwf_panel_get_vcom(void);
char *einkwf_panel_get_vcom_str(void);


#endif /* _EINKWF_H */
