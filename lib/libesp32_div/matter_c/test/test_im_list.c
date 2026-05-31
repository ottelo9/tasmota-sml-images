#include <stdio.h>
#include <string.h>
#include "mtrc_im.h"
#include "mtrc_tlv.h"
static int ok=1;
static void chk(const char*l,int c){printf("  [%s] %s\n",c?"PASS":"FAIL",l);ok&=c;}
// Find Data array at ctx2 after the AttributePathIB; collect its uints.
static int extract_list(const uint8_t*b,int n,uint32_t*out,int max){
  mtrc_tlv_reader r; mtrc_tlv_reader_init(&r,b,n); mtrc_tlv_elem e;
  // walk to the ctx2 ARRAY that is the Data (after a LIST=path closed)
  int cnt=-1; 
  while(mtrc_tlv_read(&r,&e)){
    if(e.type==MTRC_TLV_ARRAY && e.tag.ctrl==MTRC_TLV_TAG_CONTEXT && e.tag.number==2){
      cnt=0; mtrc_tlv_elem x;
      while(mtrc_tlv_read(&r,&x) && x.type!=MTRC_TLV_END){
        if(x.type==MTRC_TLV_UINT && cnt<max) out[cnt++]=(uint32_t)x.u;
      }
      break;
    }
  }
  return cnt;
}
int main(void){
  uint8_t buf[256]; uint32_t got[16];
  printf("Descriptor list report builders\n");
  uint32_t srv[3]={0x0006,0x0090,0x001D};
  int n=mtrc_im_build_report_list_uint(buf,sizeof(buf),0,1,0x001D,0x0001,srv,3);
  chk("ServerList builds",n>0);
  int c=extract_list(buf,n,got,16);
  chk("ServerList count==3",c==3);
  chk("ServerList[0]==0x06",c>=1&&got[0]==0x0006);
  chk("ServerList[1]==0x90",c>=2&&got[1]==0x0090);
  chk("ServerList[2]==0x1D",c>=3&&got[2]==0x001D);
  int e=mtrc_im_build_report_list_uint(buf,sizeof(buf),0,1,0x001D,0x0002,NULL,0);
  chk("ClientList(empty) builds",e>0 && extract_list(buf,e,got,16)==0);
  int d=mtrc_im_build_report_devtypelist(buf,sizeof(buf),0,1,0x001D,0x0000,0x010A,1);
  chk("DeviceTypeList builds",d>0);
  // decode struct {0:devtype,1:rev} inside the ctx2 array
  mtrc_tlv_reader r; mtrc_tlv_reader_init(&r,buf,d); mtrc_tlv_elem el; int found=0;
  while(mtrc_tlv_read(&r,&el)){
    if(el.type==MTRC_TLV_ARRAY && el.tag.ctrl==MTRC_TLV_TAG_CONTEXT && el.tag.number==2){
      mtrc_tlv_elem s; uint32_t dt=0,rev=0;
      while(mtrc_tlv_read(&r,&s) && s.type!=MTRC_TLV_END){
        if(s.type==MTRC_TLV_STRUCT){ mtrc_tlv_elem f;
          while(mtrc_tlv_read(&r,&f)&&f.type!=MTRC_TLV_END){
            if(f.tag.number==0)dt=(uint32_t)f.u; if(f.tag.number==1)rev=(uint32_t)f.u; }
        }
      }
      found=(dt==0x010A&&rev==1);
    }
  }
  chk("DeviceTypeList {0x010A,1}",found);
  printf(ok?"\n==> list builders PASS\n":"\n==> list builders FAIL\n");
  return ok?0:1;
}
