#ifndef KW_CATALOG_H
#define KW_CATALOG_H
#define KC_CAP 2048
#define KC_PAGE_SIZE 32
/* Only metadata. Covers remain in thumbs.c's bounded 24-texture LRU. */
typedef struct {unsigned long id,group_id;int viewers,mature,gambling;char name[96],slug[96],group[48],art[768];} kc_category;
typedef struct {int page,last,total,retryable;} kc_page;
typedef struct {int started,running,done,limited,pages,total,count,gen;char error[200];} kc_status;
typedef struct {unsigned long id;char name[48];} kc_group;
int kw_categories_page(int page,kc_category *out,int cap,kc_page *meta,char *err,int esz,const volatile int *cancel);
int kc_start(void);
void kc_stop(void);
void kc_pause(int pause);
void kc_status_get(kc_status *out);
int kc_view(int *out,int cap,int show_mature,unsigned long group,const char *query);
int kc_get(int index,kc_category *out);
int kc_groups(kc_group *out,int cap,int show_mature);
#endif
