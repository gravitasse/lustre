/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#define DEBUG_SUBSYSTEM S_LOG

#ifndef EXPORT_SYMTAB
#define EXPORT_SYMTAB
#endif

#include <linux/fs.h>
#include <linux/obd_class.h>
#include <linux/lustre_log.h>
#include <portals/list.h>

static int str2logid(struct llog_logid *logid, char *str, int len)
{
        char *start, *end, *endp;

        start = str;
        if (*start != '#')
                RETURN(-EINVAL);

        start++;
        if (start - str >= len - 1)
                RETURN(-EINVAL);
        end = strchr(start, '#');
        if (end == NULL || end == start)
                RETURN(-EINVAL);

        *end = '\0';
        logid->lgl_oid = simple_strtoull(start, &endp, 0);
        if (endp != end)
                RETURN(-EINVAL);

        start = ++end;
        if (start - str >= len - 1)
                RETURN(-EINVAL);
        end = strchr(start, '#');
        if (end == NULL || end == start)
                RETURN(-EINVAL);

        *end = '\0';
        logid->lgl_ogr = simple_strtoull(start, &endp, 0);
        if (endp != end)
                RETURN(-EINVAL);

        start = ++end;
        if (start - str >= len - 1)
                RETURN(-EINVAL);
        logid->lgl_ogen = simple_strtoul(start, &endp, 16);
        if (*endp != '\0')
                RETURN(-EINVAL);

        RETURN(0);
}

static int llog_check_cb(struct llog_handle *handle, struct llog_rec_hdr *rec,
                         void *data)
{
        struct obd_ioctl_data *ioc_data = (struct obd_ioctl_data *)data;
        static int l, remains, from, to;
        static char *out;
        char *endp;
        int cur_index, rc = 0;

        cur_index = rec->lrh_index;

        if (ioc_data && (ioc_data->ioc_inllen1)) {
                l = 0;
                remains = ioc_data->ioc_inllen4 +
                        size_round(ioc_data->ioc_inllen1) +
                        size_round(ioc_data->ioc_inllen2) +
                        size_round(ioc_data->ioc_inllen3);
                from = simple_strtol(ioc_data->ioc_inlbuf2, &endp, 0);
                if (*endp != '\0')
                        RETURN(-EINVAL);
                to = simple_strtol(ioc_data->ioc_inlbuf3, &endp, 0);
                if (*endp != '\0')
                        RETURN(-EINVAL);
                ioc_data->ioc_inllen1 = 0;
                out = ioc_data->ioc_bulk;
                if (cur_index < from)
                        RETURN(0);
                if (to > 0 && cur_index > to)
                        RETURN(-LLOG_EEMPTY);
        }
        if (handle->lgh_hdr->llh_flags & LLOG_F_IS_CAT) {
                struct llog_logid_rec *lir = (struct llog_logid_rec *)rec;
                struct llog_handle *log_handle;

                if (rec->lrh_type != LLOG_LOGID_MAGIC) {
                        l = snprintf(out, remains, "[index]: %05d  [type]: "
                                     "%02x  [len]: %04d failed\n",
                                     cur_index, rec->lrh_type,
                                     rec->lrh_len);
                }
                if (handle->lgh_ctxt == NULL)
                        RETURN(-EOPNOTSUPP);
                llog_cat_id2handle(handle, &log_handle, &lir->lid_id);
                rc = llog_process(log_handle, llog_check_cb, NULL, NULL);
                llog_close(log_handle);
        } else {
                switch (rec->lrh_type) {
                case OST_SZ_REC:
                case OST_RAID1_REC:
                case MDS_UNLINK_REC:
                case OBD_CFG_REC:
                case PTL_CFG_REC:
                case LLOG_HDR_MAGIC: {
                         l = snprintf(out, remains, "[index]: %05d  [type]: "
                                      "%02x  [len]: %04d ok\n",
                                      cur_index, rec->lrh_type,
                                      rec->lrh_len);
                         out += l;
                         remains -= l;
                         if (remains <= 0) {
                                CERROR("no space to print log records\n");
                                RETURN(-LLOG_EEMPTY);
                         }
                         RETURN(0);
                }
                default: {
                         l = snprintf(out, remains, "[index]: %05d  [type]: "
                                      "%02x  [len]: %04d failed\n",
                                      cur_index, rec->lrh_type,
                                      rec->lrh_len);
                         out += l;
                         remains -= l;
                         if (remains <= 0) {
                                CERROR("no space to print log records\n");
                                RETURN(-LLOG_EEMPTY);
                         }
                         RETURN(0);
                }
                }
        }
        RETURN(rc);
}

static int llog_print_cb(struct llog_handle *handle, struct llog_rec_hdr *rec,
                         void *data)
{
        struct obd_ioctl_data *ioc_data = (struct obd_ioctl_data *)data;
        static int l, remains, from, to;
        static char *out;
        char *endp;
        int cur_index;

        if (ioc_data->ioc_inllen1) {
                l = 0;
                remains = ioc_data->ioc_inllen4 +
                        size_round(ioc_data->ioc_inllen1) +
                        size_round(ioc_data->ioc_inllen2) +
                        size_round(ioc_data->ioc_inllen3);
                from = simple_strtol(ioc_data->ioc_inlbuf2, &endp, 0);
                if (*endp != '\0')
                        RETURN(-EINVAL);
                to = simple_strtol(ioc_data->ioc_inlbuf3, &endp, 0);
                if (*endp != '\0')
                        RETURN(-EINVAL);
                out = ioc_data->ioc_bulk;
                ioc_data->ioc_inllen1 = 0;
        }

        cur_index = rec->lrh_index;
        if (cur_index < from)
                RETURN(0);
        if (to > 0 && cur_index > to)
                RETURN(-LLOG_EEMPTY);

        if (handle->lgh_hdr->llh_flags & LLOG_F_IS_CAT) {
                struct llog_logid_rec *lir = (struct llog_logid_rec *)rec;
                if (rec->lrh_type != LLOG_LOGID_MAGIC) {
                        CERROR("invalid record in catalog\n");
                        RETURN(-EINVAL);
                }

                l = snprintf(out, remains,
                             "[index]: %05d  [logid]: #"LPX64"#"LPX64"#%08x\n",
                             cur_index, lir->lid_id.lgl_oid,
                             lir->lid_id.lgl_ogr, lir->lid_id.lgl_ogen);
        } else {
                l = snprintf(out, remains,
                             "[index]: %05d  [type]: %02x  [len]: %04d\n",
                             cur_index, rec->lrh_type,
                             rec->lrh_len);
        }
        out += l;
        remains -= l;
        if (remains <= 0) {
                CERROR("not enough space for print log records\n");
                RETURN(-LLOG_EEMPTY);
        }

        RETURN(0);
}
static int llog_remove_log(struct llog_handle *cat, struct llog_logid *logid)
{
        struct llog_handle *log;
        int rc, index = 0;

        down_write(&cat->lgh_lock);
        rc = llog_cat_id2handle(cat, &log, logid);
        if (rc) {
                CDEBUG(D_IOCTL, "cannot find log #"LPX64"#"LPX64"#%08x\n",
                       logid->lgl_oid, logid->lgl_ogr, logid->lgl_ogen);
                GOTO(out, rc = -ENOENT);
        }

        index = log->u.phd.phd_cookie.lgc_index;
        LASSERT(index);
        rc = llog_destroy(log);
        if (rc) {
                CDEBUG(D_IOCTL, "cannot destroy log\n");
                GOTO(out, rc);
        }
        llog_cat_set_first_idx(cat, index);
        rc = llog_cancel_rec(cat, index);
out:
        llog_free_handle(log);
        up_write(&cat->lgh_lock);
        RETURN(rc);

}

static int llog_delete_cb(struct llog_handle *handle, struct llog_rec_hdr *rec,
                          void *data)
{
        struct  llog_logid_rec *lir = (struct llog_logid_rec*)rec;
        int     rc;

        if (rec->lrh_type != LLOG_LOGID_MAGIC)
              return (-EINVAL);
        rc = llog_remove_log(handle, &lir->lid_id);

        RETURN(rc);
}


int llog_ioctl(struct llog_ctxt *ctxt, int cmd, struct obd_ioctl_data *data)
{
        struct llog_logid logid;
        int err = 0;
        struct llog_handle *handle = NULL;

        if (*data->ioc_inlbuf1 == '#') {
                err = str2logid(&logid, data->ioc_inlbuf1, data->ioc_inllen1);
                if (err)
                        GOTO(out, err);
                err = llog_create(ctxt, &handle, &logid, NULL);
                if (err)
                        GOTO(out, err);
        } else if (*data->ioc_inlbuf1 == '$') {
                char *name = data->ioc_inlbuf1 + 1;
                err = llog_create(ctxt, &handle, NULL, name);
                if (err)
                        GOTO(out, err);
        } else {
                GOTO(out, err = -EINVAL);
        }

        err = llog_init_handle(handle, 0, NULL);
        if (err)
                GOTO(out_close, err = -ENOENT);

        switch (cmd) {
        case OBD_IOC_LLOG_INFO: {
                int l;
                int remains = data->ioc_inllen2 +
                        size_round(data->ioc_inllen1);
                char *out = data->ioc_bulk;

                l = snprintf(out, remains,
                             "logid:            #"LPX64"#"LPX64"#%08x\n"
                             "flags:            %x (%s)\n"
                             "records count:    %d\n"
                             "last index:       %d\n",
                             handle->lgh_id.lgl_oid, handle->lgh_id.lgl_ogr,
                             handle->lgh_id.lgl_ogen,
                             handle->lgh_hdr->llh_flags,
                             handle->lgh_hdr->llh_flags &
                             LLOG_F_IS_CAT ? "cat" : "plain",
                             handle->lgh_hdr->llh_count,
                             handle->lgh_last_idx);
                out += l;
                remains -= l;
                if (remains <= 0)
                        CERROR("not enough space for log header info\n");

                GOTO(out_close, err);
        }
        case OBD_IOC_LLOG_CHECK: {
                LASSERT(data->ioc_inllen1);
                err = llog_process(handle, llog_check_cb, data, NULL);
                if (err == -LLOG_EEMPTY)
                        err = 0;
                GOTO(out_close, err);
        }

        case OBD_IOC_LLOG_PRINT: {
                LASSERT(data->ioc_inllen1);
                err = llog_process(handle, llog_print_cb, data, NULL);
                if (err == -LLOG_EEMPTY)
                        err = 0;

                GOTO(out_close, err);
        }
        case OBD_IOC_LLOG_CANCEL: {
                struct llog_cookie cookie;
                struct llog_logid plain;
                char *endp;

                cookie.lgc_index = simple_strtoul(data->ioc_inlbuf3, &endp, 0);
                if (*endp != '\0')
                        GOTO(out_close, err = -EINVAL);

                if (handle->lgh_hdr->llh_flags & LLOG_F_IS_CAT) {
                        down_write(&handle->lgh_lock);
                        err = llog_cancel_rec(handle, cookie.lgc_index);
                        up_write(&handle->lgh_lock);
                        GOTO(out_close, err);
                }

                err = str2logid(&plain, data->ioc_inlbuf2, data->ioc_inllen2);
                if (err)
                        GOTO(out_close, err);
                cookie.lgc_lgl = plain;

                if (!(handle->lgh_hdr->llh_flags & LLOG_F_IS_CAT))
                        GOTO(out_close, err = -EINVAL);

                err = llog_cat_cancel_records(handle, 1, &cookie);
                GOTO(out_close, err);
        }
        case OBD_IOC_LLOG_REMOVE: {
                struct llog_logid plain;

                if (handle->lgh_hdr->llh_flags & LLOG_F_IS_PLAIN) {
                        err = llog_destroy(handle);
                        if (!err)
                                llog_free_handle(handle);
                        GOTO(out, err);
                }

                if (!(handle->lgh_hdr->llh_flags & LLOG_F_IS_CAT))
                        GOTO(out_close, err = -EINVAL);

                if (data->ioc_inlbuf2) {
                        /*remove indicate log from the catalog*/
                        err = str2logid(&plain, data->ioc_inlbuf2,
                                        data->ioc_inllen2);
                        if (err)
                                GOTO(out_close, err);
                        err = llog_remove_log(handle, &plain);
                } else {
                        /*remove all the log of the catalog*/
                        llog_process(handle, llog_delete_cb, NULL, NULL);
                }
                GOTO(out_close, err);
        }
        }

out_close:
        if (handle->lgh_hdr &&
            handle->lgh_hdr->llh_flags & LLOG_F_IS_CAT)
                llog_cat_put(handle);
        else
                llog_close(handle);
out:
        RETURN(err);
}
EXPORT_SYMBOL(llog_ioctl);

int llog_catalog_list(struct obd_device *obd, int count,
                      struct obd_ioctl_data *data)
{
        int size, i;
        struct llog_catid *idarray;
        struct llog_logid *id;
        char name[32] = CATLIST;
        char *out;
        int l, remains, rc = 0;

        size = sizeof(*idarray) * count;

        OBD_ALLOC(idarray, size);
        if (!idarray)
                RETURN(-ENOMEM);
        memset(idarray, 0, size);

        rc = llog_get_cat_list(&obd->obd_lvfs_ctxt, obd->obd_fsops,
                               name, count, idarray);
        if (rc) {
                OBD_FREE(idarray, size);
                RETURN(rc);
        }

        out = data->ioc_bulk;
        remains = data->ioc_inllen1;
        for (i = 0; i < count; i++) {
                id = &idarray[i].lci_logid;
                l = snprintf(out, remains,
                             "catalog log: #"LPX64"#"LPX64"#%08x\n",
                             id->lgl_oid, id->lgl_ogr, id->lgl_ogen);
                out += l;
                remains -= l;
                if (remains <= 0) {
                        CWARN("not enough memory for catlog list\n");
                        break;
                }
        }
        OBD_FREE(idarray, size);
        RETURN(0);

}
EXPORT_SYMBOL(llog_catalog_list);
