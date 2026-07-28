#ifndef __SCREEN_H
#define __SCREEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void Disp_HZ16str(uint8_t uc_RowNo, uint8_t uc_ColNo,
                  const uint8_t *HZinf,
                  uint8_t uc_HZCnt);
void ScreenInit(void);
void Clear_Screen(void);

#ifdef __cplusplus
}
#endif

#endif
