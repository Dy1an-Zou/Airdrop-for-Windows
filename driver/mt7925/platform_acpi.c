/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * ACPI_EVAL_OUTPUT_BUFFER_V1 / ACPI_METHOD_ARGUMENT_V1 wire format from the
 * Microsoft WDK. Fixed MTDS/MTGS table semantics are implemented in tx_init.c.
 */
#include "platform_acpi.h"
static uint32_t le16(const uint8_t *p) {return (uint32_t)p[0]|(uint32_t)p[1]<<8;}
static uint32_t le32(const uint8_t *p) {return le16(p)|le16(p+2)<<16;}
static void clear(mt_acpi_table *t) {
    uint32_t i;
    t->state=0;t->ntstatus=0;t->size=0;
    for(i=0;i<MT_ACPI_TABLE_BYTES;++i)t->data[i]=0;
}
int mt_acpi_decode(uint32_t status,const uint8_t *p,uint32_t size,mt_acpi_table *out) {
    uint32_t count,i;
    if(!out)return MT_INVALID;
    clear(out);out->ntstatus=status;
    if(status==MT_ACPI_NAME_NOT_FOUND) {
        if(size)return MT_INVALID;
        out->state=MT_ACPI_ABSENT;return MT_OK;
    }
    if(status || !p || size<12 || size>MT_ACPI_WIRE_BYTES ||
        le32(p)!=MT_ACPI_OUTPUT_SIGNATURE || le32(p+4)!=size)return MT_INVALID;
    count=le32(p+8);
    if(count<4 || count>MT_ACPI_TABLE_BYTES || size!=12+count*8)return MT_INVALID;
    /* Check the entire result before publishing even the first table byte. */
    for(i=0;i<count;++i) {
        const uint8_t *arg=p+12+i*8;
        if(le16(arg)!=0 || le16(arg+2)!=4 || le32(arg+4)>255)return MT_INVALID;
    }
    for(i=0;i<count;++i)out->data[i]=p[16+i*8];
    out->size=count;out->state=MT_ACPI_DATA;return MT_OK;
}
int mt_platform_read(const mt_acpi_io *io,mt_platform *out) {
    static const uint8_t names[2][4]={{'M','T','D','S'},{'M','T','G','S'}};
    uint8_t wire[MT_ACPI_WIRE_BYTES];
    uint32_t i,j,size,status;
    int error;
    if(!out)return MT_INVALID;
    out->size=sizeof(*out);out->version=1;out->error=MT_INVALID;out->reserved=0;
    clear(&out->dynamic);clear(&out->geographic);
    if(!io || !io->evaluate)return MT_INVALID;
    out->error=MT_OK;
    for(i=0;i<2;++i) {
        for(j=0;j<MT_ACPI_WIRE_BYTES;++j)wire[j]=0;
        size=0;
        status=(uint32_t)io->evaluate(io->context,names[i],wire,MT_ACPI_WIRE_BYTES,&size);
        error=mt_acpi_decode(status,wire,size,i?&out->geographic:&out->dynamic);
        if(error && !out->error)out->error=error;
    }
    return out->error;
}
static int valid(const mt_acpi_table *t) {
    if(t->state==MT_ACPI_ABSENT)return t->ntstatus==MT_ACPI_NAME_NOT_FOUND && !t->size;
    return t->state==MT_ACPI_DATA && !t->ntstatus && t->size>=4 && t->size<=MT_ACPI_TABLE_BYTES;
}
int mt_platform_policy(const mt_platform *p,uint32_t region,int32_t cap,mt_tx_policy *out) {
    if(!p || p->size!=sizeof(*p) || p->version!=1 || p->error || p->reserved ||
        !valid(&p->dynamic) || !valid(&p->geographic)) {
        /* Use the policy's own invalidation path, including for a stale caller output. */
        return mt_tx_policy_prepare(out,0,0,0,0,0,0,region,cap);
    }
    return mt_tx_policy_prepare(out,p->dynamic.state,
        p->dynamic.state==MT_ACPI_DATA?p->dynamic.data:0,p->dynamic.size,
        p->geographic.state,p->geographic.state==MT_ACPI_DATA?p->geographic.data:0,
        p->geographic.size,region,cap);
}
