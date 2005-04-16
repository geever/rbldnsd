/* $Id$
 * Nameserver zones: structures and routines
 */

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#include "rbldnsd.h"

static struct dataset *ds_list;
struct dataset *g_dsacl;

static struct dataset *newdataset(char *spec) {
  /* type:file,file,file... */
  struct dataset *ds, **dsp;
  char *f;
  struct dsfile **dsfp, *dsf;
  static const char *const delims = ",:";
  const struct dstype **dstp;

  f = strchr(spec, ':');
  if (!f)
    error(0, "invalid zone data specification `%.60s'", spec);
  *f++ = '\0';

  for(dsp = &ds_list; (ds = *dsp) != NULL; dsp = &ds->ds_next)
    if (strcmp(ds->ds_type->dst_name, spec) == 0 &&
        strcmp(ds->ds_spec, f) == 0)
      return ds;

  dstp = ds_types;
  while(strcmp(spec, (*dstp)->dst_name))
    if (!*++dstp)
      error(0, "unknown dataset type `%.60s'", spec);
  ds = (struct dataset*)ezalloc(sizeof(struct dataset) +
                                     sizeof(struct mempool) +
                                     (*dstp)->dst_size);
  ds->ds_type = *dstp;
  ds->ds_mp = (struct mempool*)(ds + 1);
  ds->ds_dsd = (struct dsdata*)(ds->ds_mp + 1);
  ds->ds_spec = estrdup(f);

  ds->ds_next = NULL;
  *dsp = ds;

  dsfp = &ds->ds_dsf;
  for (f = strtok(f, delims); f; f = strtok(NULL, delims)) {
    dsf = tmalloc(struct dsfile);
    dsf->dsf_stamp = 0;
    dsf->dsf_name = estrdup(f);
    *dsfp = dsf;
    dsfp = &dsf->dsf_next;
  }
  *dsfp = NULL;
  if (!ds->ds_dsf)
    error(0, "missing filenames for %s", spec);

  return ds;
}

struct zone *newzone(struct zone **zonelist,
                     unsigned char *dn, unsigned dnlen,
                     struct mempool *mp) {
  struct zone *zone, **zonep, **lastzonep;
 
  zonep = zonelist;
  lastzonep = NULL;

  for (;;) {
    if (!(zone = *zonep)) {
      if (mp)
        zone = mp_talloc(mp, struct zone);
      else
        zone = tmalloc(struct zone);
      if (!zone)
        return NULL;
      memset(zone, 0, sizeof(*zone));
      if (lastzonep) { zone->z_next = *lastzonep; *lastzonep = zone; }
      else *zonep = zone;
      memcpy(zone->z_dn, dn, dnlen);
      zone->z_dnlen = dnlen;
      zone->z_dnlab = dns_dnlabels(dn);
      zone->z_dslp = &zone->z_dsl;
      break;
    }
    else if (zone->z_dnlen == dnlen && memcmp(zone->z_dn, dn, dnlen) == 0)
      break;
    else {
      if (!lastzonep && zone->z_dnlen < dnlen &&
          memcmp(dn + dnlen - zone->z_dnlen, zone->z_dn, zone->z_dnlen) == 0)
        lastzonep = zonep;
      zonep = &zone->z_next;
    }
  }

  return zone;
}

void connectdataset(struct zone *zone,
                    struct dataset *ds,
                    struct dslist *dsl) {
  dsl->dsl_next = NULL;
  *zone->z_dslp = dsl;
  zone->z_dslp = &dsl->dsl_next;
  dsl->dsl_ds = ds;
  dsl->dsl_queryfn = ds->ds_type->dst_queryfn;
  zone->z_dstflags |= ds->ds_type->dst_flags;
}

struct zone *addzone(struct zone *zonelist, const char *spec) {
  struct zone *zone;
  char *p;
  char name[DNS_MAXDOMAIN];
  unsigned char dn[DNS_MAXDN];
  unsigned dnlen;
  struct dataset *ds;

  p = strchr(spec, ':');
  if (!p || p - spec >= DNS_MAXDOMAIN)
    error(0, "invalid zone spec `%.60s'", spec);

  memcpy(name, spec, p - spec);
  name[p - spec] = '\0';

  dnlen = dns_ptodn(name, dn, sizeof(dn));
  if (!dnlen)
    error(0, "invalid domain name `%.80s'", name);

  p = estrdup(p+1);
  ds = newdataset(p);

  if (!dn[0]) {
    if (!isdstype(ds->ds_type, acl))
      error(0, "missing domain name in `%.60s'", spec);
    if (g_dsacl)
      error(0, "global acl specified more than once");
    g_dsacl = ds;
  }
  else {
    zone = newzone(&zonelist, dn, dnlen, NULL);
    if (isdstype(ds->ds_type, acl)) {
      if (zone->z_dsacl)
        error(0, "repeated ACL definition for zone `%.60s'", name);
      zone->z_dsacl = ds;
    }
    else
      connectdataset(zone, ds, tmalloc(struct dslist));
  }
  free(p);

  return zonelist;
}

/* parse $SPECIAL construct */
int ds_special(struct dataset *ds, char *line, struct dsctx *dsc) {

  switch(*line) {

  case 's': case 'S':

  if ((line[1] == 'o' || line[1] == 'O') &&
      (line[2] == 'a' || line[2] == 'A') &&
      ISSPACE(line[3]) && !isdstype(ds->ds_type, acl)) {

    /* SOA record */
    struct dssoa dssoa;
    unsigned char odn[DNS_MAXDN], pdn[DNS_MAXDN];
    unsigned odnlen, pdnlen;

    if (ds->ds_dssoa)
      return 1; /* ignore if already set */

    line += 4;
    SKIPSPACE(line);

    if (!(line = parse_ttl(line, &dssoa.dssoa_ttl, ds->ds_ttl))) return 0;
    if (!(line = parse_dn(line, odn, &odnlen))) return 0;
    if (!(line = parse_dn(line, pdn, &pdnlen))) return 0;
    if (!(line = parse_uint32(line, &dssoa.dssoa_serial))) return 0;
    if (!(line = parse_time_nb(line, dssoa.dssoa_n+0))) return 0;
    if (!(line = parse_time_nb(line, dssoa.dssoa_n+4))) return 0;
    if (!(line = parse_time_nb(line, dssoa.dssoa_n+8))) return 0;
    if (!(line = parse_time_nb(line, dssoa.dssoa_n+12))) return 0;
    if (*line) return 0;

    dssoa.dssoa_odn = mp_memdup(ds->ds_mp, odn, odnlen);
    dssoa.dssoa_pdn = mp_memdup(ds->ds_mp, pdn, pdnlen);
    if (!dssoa.dssoa_odn || !dssoa.dssoa_pdn) return -1;
    ds->ds_dssoa = mp_talloc(ds->ds_mp, struct dssoa);
    if (!ds->ds_dssoa) return -1;
    *ds->ds_dssoa = dssoa;

    return 1;
  }
  break;

  case 'n': case 'N':

  if ((line[1] == 's' || line[1] == 'S') &&
      ISSPACE(line[2]) && !isdstype(ds->ds_type, acl)) {

     unsigned char dn[DNS_MAXDN];
     unsigned dnlen;
     struct dsns *dsns, **dsnslp;
     unsigned ttl;

#ifndef INCOMPAT_0_99
#warning NS record compatibility mode: remove for 1.0 final
     struct dsns *dsns_first = 0;
     unsigned cnt;
     int newformat = 0;
#endif

#ifndef INCOMPAT_0_99
     if (ds->ds_nsflags & DSF_NEWNS) return 1;
     if (ds->ds_dsns) {
       dsns = ds->ds_dsns;
       while(dsns->dsns_next)
         dsns = dsns->dsns_next;
       dsnslp = &dsns->dsns_next;
     }
     else
       dsnslp = &ds->ds_dsns;
     cnt = 0;
#else
     if (ds->ds_dsns) return 1; /* ignore 2nd nameserver line */
     dsnslp = &ds->ds_dsns;
#endif

     line += 3;
     SKIPSPACE(line);

     /*XXX parse options (AndrewSN suggested `-bloat') here */

     if (!(line = parse_ttl(line, &ttl, ds->ds_ttl))) return 0;

     do {
       if (*line == '-') {
         /* skip nameservers that start with `-' aka 'commented-out' */
         do ++line; while (*line && !ISSPACE(*line));
         SKIPSPACE(line);
#ifndef INCOMPAT_0_99
	 newformat = 1;
#endif
         continue;
       }
       if (!(line = parse_dn(line, dn, &dnlen))) return 0;
       dsns = (struct dsns*)
         mp_alloc(ds->ds_mp, sizeof(struct dsns) + dnlen - 1, 1);
       if (!dsns) return -1;
       memcpy(dsns->dsns_dn, dn, dnlen);
       *dsnslp = dsns;
       dsnslp = &dsns->dsns_next;
       *dsnslp = NULL;
#ifndef INCOMPAT_0_99
       if (!cnt++)
         dsns_first = dsns;
#endif
     } while(*line);

#ifndef INCOMPAT_0_99
     if (cnt > 1 || newformat) {
       ds->ds_nsflags |= DSF_NEWNS;
       ds->ds_dsns = dsns_first; /* throw away all NS recs */
     }
     else if (dsns_first != ds->ds_dsns && !(ds->ds_nsflags & DSF_NSWARN)) {
       dswarn(dsc, "compatibility mode: specify all NS records in ONE line");
       ds->ds_nsflags |= DSF_NSWARN;
     }
     if (!ds->ds_nsttl || ds->ds_nsttl > ttl)
       ds->ds_nsttl = ttl;
#else
     ds->ds_nsttl = ttl;
#endif

     return 1;
  }
  break;

  case 't': case 'T':
  if ((line[1] == 't' || line[1] == 'T') &&
      (line[2] == 'l' || line[2] == 'L') &&
      ISSPACE(line[3])) {
    unsigned ttl;
    line += 4;
    SKIPSPACE(line);
    if (!(line = parse_ttl(line, &ttl, def_ttl))) return 0;
    if (*line) return 0;
    if (dsc->dsc_subset) dsc->dsc_subset->ds_ttl = ttl;
    else ds->ds_ttl = ttl;
    return 1;
  }
  break;

  case 'm': case 'M':
  if ((line[1] == 'A' || line[1] == 'a') &&
      (line[2] == 'X' || line[2] == 'x') &&
      (line[3] == 'R' || line[3] == 'r') &&
      (line[4] == 'A' || line[4] == 'a') &&
      (line[5] == 'N' || line[5] == 'n') &&
      (line[6] == 'G' || line[6] == 'g') &&
      (line[7] == 'E' || line[7] == 'e') &&
      line[8] == '4' && ISSPACE(line[9])) {
    unsigned r;
    int cidr;
    line += 10; SKIPSPACE(line);
    if (*line == '/') cidr = 1, ++line;
    else cidr = 0;
    if (!(line = parse_uint32(line, &r)) || *line || !r)
      return 0;
    if (cidr) {
      if (r > 32) return 0;
      r = ~ip4mask(r) + 1;
    }
    if (dsc->dsc_ip4maxrange && dsc->dsc_ip4maxrange < r)
      dswarn(dsc, "ignoring attempt to increase $MAXRANGE4 from %u to %u",
             dsc->dsc_ip4maxrange, r);
    else
      dsc->dsc_ip4maxrange = r;
    return 1;
  }

  case '0': case '1': case '2': case '3': case '4':
  case '5': case '6': case '7': case '8': case '9':
  if (ISSPACE(line[1])) {
    /* substitution vars */
    unsigned n = line[0] - '0';
    if (dsc->dsc_subset) ds = dsc->dsc_subset;
    if (ds->ds_subst[n]) return 1; /* ignore second assignment */
    line += 2;
    SKIPSPACE(line);
    if (!*line) return 0;
    if (!(ds->ds_subst[n] = mp_strdup(ds->ds_mp, line))) return 0;
    return 1;
  }
  break;

  case 'd': case 'D':
  if ((line[1] == 'A' || line[1] == 'a') &&
      (line[2] == 'T' || line[2] == 't') &&
      (line[3] == 'A' || line[3] == 'a') &&
      (line[4] == 'S' || line[4] == 's') &&
      (line[5] == 'E' || line[5] == 'e') &&
      (line[6] == 'T' || line[6] == 't') &&
      ISSPACE(line[7]) &&
      isdstype(ds->ds_type, combined)) {
    line += 8;
    SKIPSPACE(line);
    return ds_combined_newset(ds, line, dsc);
  }
  break;

  }

  return 0;
}

static void freedataset(struct dataset *ds) {
  ds->ds_type->dst_resetfn(ds->ds_dsd, 0);
  mp_free(ds->ds_mp);
  ds->ds_dssoa = NULL;
  ds->ds_ttl = def_ttl;
  ds->ds_dsns = NULL;
  ds->ds_nsttl = 0;
#ifndef INCOMPAT_0_99
  ds->ds_nsflags = 0;
#endif
  memset(ds->ds_subst, 0, sizeof(ds->ds_subst));
}

static int loaddataset(struct dataset *ds) {
  struct dsfile *dsf;
  time_t stamp = 0;
  FILE *f;
  struct stat st0, st1;
  struct dsctx dsc;

  freedataset(ds);

  memset(&dsc, 0, sizeof(dsc));
  dsc.dsc_ds = ds;

  for(dsf = ds->ds_dsf; dsf; dsf = dsf->dsf_next) {
    dsc.dsc_fname = dsf->dsf_name;
    f = fopen(dsf->dsf_name, "r");
    if (!f || fstat(fileno(f), &st0) < 0) {
      dslog(LOG_ERR, &dsc, "unable to open file: %s", strerror(errno));
      if (f) fclose(f);
      return 0;
    }
    ds->ds_type->dst_startfn(ds);
    if (!readdslines(f, ds, &dsc)) {
      fclose(f);
      return 0;
    }
    dsc.dsc_lineno = 0;
    if (ferror(f) || fstat(fileno(f), &st1) < 0) {
      dslog(LOG_ERR, &dsc, "error reading file: %s", strerror(errno));
      fclose(f);
      return 0;
    }
    fclose(f);
    if (st0.st_mtime != st1.st_mtime ||
        st0.st_size  != st1.st_size) {
      dslog(LOG_ERR, &dsc,
            "file changed while we where reading it, data load aborted");
      dslog(LOG_ERR, &dsc,
            "do not write data files directly, "
            "use temp file and rename(2) instead");
      return 0;
    }
    dsf->dsf_stamp = st0.st_mtime;
    dsf->dsf_size  = st0.st_size;
    if (dsf->dsf_stamp > stamp)
      stamp = dsf->dsf_stamp;
  }
  ds->ds_stamp = stamp;
  dsc.dsc_fname = NULL;

  ds->ds_type->dst_finishfn(ds, &dsc);

  return 1;
}

static int updatezone(struct zone *zone) {
  time_t stamp = 0;
  const struct dssoa *dssoa = NULL;
  const struct dsns *dsns = NULL;
  unsigned nsttl = 0;
  struct dslist *dsl;

  for(dsl = zone->z_dsl; dsl; dsl = dsl->dsl_next) {
    const struct dataset *ds = dsl->dsl_ds;
    if (!ds->ds_stamp)
      return 0;
    if (stamp < ds->ds_stamp)
      stamp = ds->ds_stamp;
    if (!dssoa)
      dssoa = ds->ds_dssoa;
    if (!dsns)
      dsns = ds->ds_dsns, nsttl = ds->ds_nsttl;
  }

  zone->z_stamp = stamp;
  if (!update_zone_soa(zone, dssoa) ||
      !update_zone_ns(zone, dsns, nsttl))
    zlog(LOG_WARNING, zone,
         "NS or SOA RRs are too long, will be ignored");

  return 1;
}

int reloadzones(struct zone *zonelist) {
  struct dataset *ds;
  struct dsfile *dsf;
  int reloaded = 0;
  int errors = 0;
  extern void start_loading();

  for(ds = ds_list; ds; ds = ds->ds_next) {
    int load = 0;

    for(dsf = ds->ds_dsf; dsf; dsf = dsf->dsf_next) {
      struct stat st;
      if (stat(dsf->dsf_name, &st) < 0) {
        dslog(LOG_ERR, 0, "unable to stat file `%.60s': %s",
              dsf->dsf_name, strerror(errno));
        load = -1;
        break;
      }
      else if (dsf->dsf_stamp != st.st_mtime ||
               dsf->dsf_size  != st.st_size) {
        load = 1;
        dsf->dsf_stamp = st.st_mtime;
        dsf->dsf_size = st.st_size;
      }
    }

    if (!load)
      continue;

    ++reloaded;

    if (load < 0 && !ds->ds_stamp) {
      ++errors;
      continue;
    }

    start_loading();

    if (load < 0 || !loaddataset(ds)) {
      ++errors;
      freedataset(ds);
      for (dsf = ds->ds_dsf; dsf; dsf = dsf->dsf_next)
        dsf->dsf_stamp = 0;
      ds->ds_stamp = 0;
    }

  }

  if (reloaded) {

    for(; zonelist; zonelist = zonelist->z_next) {
      if (!updatezone(zonelist)) {
        zlog(LOG_WARNING, zonelist, "zone will not be serviced");
        zonelist->z_stamp = 0;
      }
    }

  }

  return errors ? -1 : reloaded ? 1 : 0;
}

#ifndef NO_MASTER_DUMP
void dumpzone(const struct zone *z, FILE *f) {
  const struct dslist *dsl;
  { /* zone header */
    char name[DNS_MAXDOMAIN+1];
    const unsigned char **nsdna = z->z_nsdna;
    const struct dssoa *dssoa = z->z_dssoa;
    unsigned nns = z->z_nns;
    unsigned n;
    dns_dntop(z->z_dn, name, sizeof(name));
    fprintf(f, "$ORIGIN\t%s.\n", name);
    if (z->z_dssoa) {
      fprintf(f, "@\t%u\tSOA", dssoa->dssoa_ttl);
      dns_dntop(dssoa->dssoa_odn, name, sizeof(name));
      fprintf(f, "\t%s.", name);
      dns_dntop(dssoa->dssoa_pdn, name, sizeof(name));
      fprintf(f, "\t%s.", name);
      fprintf(f, "\t(%u %u %u %u %u)\n",
          dssoa->dssoa_serial ? dssoa->dssoa_serial : z->z_stamp,
          unpack32(dssoa->dssoa_n+0),
          unpack32(dssoa->dssoa_n+4),
          unpack32(dssoa->dssoa_n+8),
          unpack32(dssoa->dssoa_n+12));
    }
    for(n = 0; n < nns; ++n) {
      dns_dntop(nsdna[n], name, sizeof(name));
      fprintf(f, "\t%u\tNS\t%s.\n", z->z_nsttl, name);
    }
  }
  for (dsl = z->z_dsl; dsl; dsl = dsl->dsl_next) {
    fprintf(f, "$TTL %u\n", dsl->dsl_ds->ds_ttl);
    dsl->dsl_ds->ds_type->dst_dumpfn(dsl->dsl_ds, z->z_dn, f);
  }
}
#endif
