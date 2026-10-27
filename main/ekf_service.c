#include "ekf_service.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
_Static_assert(sizeof(mulEkfStream_t) <= 32768, "EKF arena exceeds reviewed ESP budget");
static bool number(const char *s, uint64_t maximum, uint64_t *out) {
  uint64_t n = 0;
  if (!*s)
    return false;
  for (; *s; s++) {
    unsigned d = (unsigned)(*s - '0');
    if (d > 9 || d > maximum || n > (maximum - d) / 10)
      return false;
    n = n * 10 + d;
  }
  *out = n;
  return true;
}
static bool real(const char *s, double *out) {
  if (!*s)
    return false;
  char *end;
  errno = 0;
  double value = strtod(s, &end);
  if (*end || errno || !isfinite(value))
    return false;
  *out = value;
  return true;
}
static void result(char *out, const ekf_service_t *s, mulEkfStatus_t status) {
  if (!s->stream) {
    snprintf(out, EKF_SERVICE_LINE, "EKF1 status=%s enabled=0", mulEkfStatusName(status));
    return;
  }
  const mulEkfStream_t *f = s->stream;
  const mulEkf_t *e = &f->current;
  snprintf(out, EKF_SERVICE_LINE,
           "EKF1 status=%s enabled=%u healthy=%u rev=%" PRIu32 " t_us=%" PRIu64
           " x=%.9g y=%.9g theta=%.9g updates=%" PRIu32 " gated=%" PRIu32
           " calibrated=0 physical_uncertainty=UNKNOWN",
           mulEkfStatusName(status), e->enabled, f->healthy, f->revision, e->timeUs, e->x[0],
           e->x[1], e->x[2], e->updates, e->gated);
}
void ekf_service_clear(ekf_service_t *s) {
  if (s) {
    free(s->stream);
    memset(s, 0, sizeof(*s));
  }
}
static unsigned nodeIndex(const mulEkfBatch_t *b, uint8_t id) {
  unsigned i = 0;
  while (i < b->anchorCount && b->anchors[i].node != id)
    i++;
  return i;
}
static int command(ekf_service_t *s, const char *input, uint32_t receivedMs,
                   char out[EKF_SERVICE_LINE]) {
  if (!s || !input || !out)
    return 0;
  if (strcmp(input, "EKF") && strncmp(input, "EKF ", 4))
    return 0;
  size_t length = strlen(input);
  if (length >= EKF_SERVICE_LINE) {
    strcpy(out, "EKF ERR oversized");
    return 1;
  }
  char line[EKF_SERVICE_LINE], *t[32];
  memcpy(line, input, length + 1);
  unsigned n = 0;
  char *at = line;
  while (*at) {
    while (*at == ' ')
      at++;
    if (!*at)
      break;
    if (n == 32)
      goto syntax;

    t[n++] = at;
    while (*at && *at != ' ') {
      if ((unsigned char)*at < 33 || (unsigned char)*at > 126)
        goto syntax;
      at++;
    }
    if (*at)
      *at++ = 0;
  }
  if (n == 1 || (n == 2 && !strcmp(t[1], "HELP"))) {
    strcpy(out,
           "EKF CONFIG EXPERIMENTAL ...; INIT ...; ON/OFF; ODOM ...; BEGIN/POSE/OBS/COVROW/COMMIT; "
           "STATUS/STATE/COV/INNOV. Aligned measured inputs only; DATA2 is not used.");
    return 1;
  }
  if (n == 2 && !strcmp(t[1], "OFF")) {
    free(s->stream);
    s->stream = NULL;
    s->assembling = false;
    result(out, s, mulEkfDisabled);
    return 1;
  }
  if (n == 2 && !strcmp(t[1], "STATUS")) {
    if (!s->stream) {
      snprintf(out, EKF_SERVICE_LINE,
               "EKF1 enabled=0 initialized=0 config=%u backend=abstract_measured DATA2_measured=0",
               mulEkfConfigValid(&s->config));
      return 1;
    }
    mulEkfStream_t *f = s->stream;
    snprintf(out, EKF_SERVICE_LINE,
             "EKF1 enabled=%u healthy=%u clock=%" PRIu32 " epoch=%" PRIu32 " frame=%" PRIu32
             " t_us=%" PRIu64 " base_us=%" PRIu64 " intervals=%u batches=%u rev=%" PRIu32
             " late=%" PRIu32 " rejected=%" PRIu32 " backend=abstract_measured DATA2_measured=0",
             f->current.enabled, f->healthy, f->current.clock, f->current.epoch, f->current.frame,
             f->current.timeUs, f->base.timeUs, f->intervalCount, f->batchCount, f->revision,
             f->late, f->rejected);
    return 1;
  }
  if (n == 16 && !strcmp(t[1], "CONFIG") && !strcmp(t[2], "EXPERIMENTAL")) {
    if (s->stream && s->stream->current.enabled) {
      strcpy(out, "EKF ERR OFF before config");
      return 1;
    }
    mulEkfConfig_t c = {.experimentalNoise = 1};
    uint64_t v;
    if (!real(t[3], &c.wheelBaseM) || !real(t[4], &c.metresPerCount) ||
        !real(t[5], &c.variancePerCountM2) || !real(t[6], &c.varianceFloorM2) ||
        !real(t[7], &c.wheelCorrelation) || !real(t[8], &c.gatePerDof))
      goto syntax;

    if (!number(t[9], UINT32_MAX, &v))
      goto syntax;
    c.maxDeltaCount = (uint32_t)v;
    if (!number(t[10], UINT32_MAX, &v))
      goto syntax;
    c.maxIntervalUs = (uint32_t)v;
    if (!number(t[11], UINT32_MAX, &v))
      goto syntax;
    c.lagUs = (uint32_t)v;
    if (!number(t[12], UINT32_MAX, &v))
      goto syntax;
    c.maxPoseSkewUs = (uint32_t)v;
    if (!number(t[13], UINT32_MAX, &v))
      goto syntax;
    c.maxTimeUncertaintyUs = (uint32_t)v;
    if (!number(t[14], 3, &v))
      goto syntax;
    c.modes = (uint8_t)v;
    if (!number(t[15], 1, &v))
      goto syntax;
    c.allowSynthetic = (uint8_t)v;
    if (!mulEkfConfigValid(&c))
      goto syntax;

    free(s->stream);
    s->stream = NULL;
    s->assembling = false;
    s->config = c;
    strcpy(out, "EKF1 config=accepted experimental=1 enabled=0 calibrated=0");
    return 1;
  }
  if (n == 5 && !strcmp(t[1], "LEVER")) {
    if ((s->stream && s->stream->current.enabled) || !mulEkfConfigValid(&s->config))
      goto syntax;

    mulEkfConfig_t c = s->config;
    if (!real(t[2], &c.leverXM) || !real(t[3], &c.leverYM) || !real(t[4], &c.heightM) ||
        !mulEkfConfigValid(&c))
      goto syntax;

    free(s->stream);
    s->stream = NULL;
    s->assembling = false;
    s->config = c;
    strcpy(out, "EKF1 lever=accepted enabled=0 INIT_required=1");
    return 1;
  }
  if (n == 12 && !strcmp(t[1], "INIT")) {
    if (s->stream && s->stream->current.enabled) {
      strcpy(out, "EKF ERR OFF before INIT");
      return 1;
    }
    uint64_t clock, epoch, frame, time;
    double x[3], p[3][3] = {{0}};
    if (!number(t[2], UINT32_MAX, &clock) || !number(t[3], UINT32_MAX, &epoch) ||
        !number(t[4], UINT32_MAX, &frame) || !number(t[5], UINT64_MAX, &time))
      goto syntax;

    for (unsigned i = 0; i < 3; i++)
      if (!real(t[6 + i], &x[i]) || !real(t[9 + i], &p[i][i]))
        goto syntax;

    if (!mulEkfConfigValid(&s->config))
      goto syntax;

    mulEkfStream_t *candidate = calloc(1, sizeof(*candidate));
    if (!candidate) {
      strcpy(out, "EKF ERR memory; disabled");
      return 1;
    }
    if (!mulEkfStreamInit(candidate, &s->config, (uint32_t)clock, (uint32_t)epoch, (uint32_t)frame,
                          time, x, p)) {
      free(candidate);
      goto syntax;
    }
    free(s->stream);
    s->stream = candidate;
    s->assembling = false;
    result(out, s, mulEkfDisabled);
    return 1;
  }
  if (n == 2 && !strcmp(t[1], "ON")) {
    result(out, s, s->stream && mulEkfStreamEnable(s->stream, true) ? mulEkfOk : mulEkfDisabled);
    return 1;
  }
  if (n == 2 && (!strcmp(t[1], "STATE") || !strcmp(t[1], "COV") || !strcmp(t[1], "INNOV"))) {
    if (!s->stream) {
      result(out, s, mulEkfDisabled);
      return 1;
    }
    const mulEkf_t *e = &s->stream->current;
    if (!strcmp(t[1], "STATE")) {
      result(out, s, mulEkfOk);
      return 1;
    }
    if (!strcmp(t[1], "COV")) {
      snprintf(out, EKF_SERVICE_LINE,
               "EKF1 type=cov rev=%" PRIu32 " t_us=%" PRIu64
               " P=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g experimental=1 physical_uncertainty=UNKNOWN",
               s->stream->revision, e->timeUs, e->p[0][0], e->p[0][1], e->p[0][2], e->p[1][1],
               e->p[1][2], e->p[2][2]);
      return 1;
    }
    int count =
        snprintf(out, EKF_SERVICE_LINE,
                 "EKF1 type=innovation group=%" PRIu64 " count=%u nis=%.9g residual=", e->lastGroup,
                 e->innovationCount, e->nis);
    for (unsigned i = 0; i < e->innovationCount && count > 0 && count < (int)EKF_SERVICE_LINE; i++)
      count += snprintf(out + count, EKF_SERVICE_LINE - (size_t)count, "%s%.9g", i ? "," : "",
                        e->innovation[i]);
    if (count < 0 || count >= (int)EKF_SERVICE_LINE)
      strcpy(out, "EKF ERR diagnostic overflow");
    return 1;
  }
  if (!s->stream || !s->stream->current.enabled) {
    result(out, s, mulEkfDisabled);
    return 1;
  }
  if (n == 11 && !strcmp(t[1], "ODOM")) {
    mulEkfOdom_t p = {0};
    uint64_t v;
    if (!number(t[2], UINT32_MAX, &v))
      goto syntax;
    p.clock = (uint32_t)v;
    if (!number(t[3], UINT32_MAX, &v))
      goto syntax;
    p.epoch = (uint32_t)v;
    if (!number(t[4], UINT64_MAX, &p.timeUs) || !number(t[5], UINT32_MAX, &v))
      goto syntax;
    p.sequence = (uint32_t)v;
    if (!number(t[6], 32, &v))
      goto syntax;
    p.bits = (uint8_t)v;
    if (!strcmp(t[7], "MEASURED"))
      p.provenance = mulOdomMeasured;
    else if (!strcmp(t[7], "SIMULATED"))
      p.provenance = mulOdomSyntheticMeasured;
    else if (!strcmp(t[7], "COMMANDED"))
      p.provenance = mulOdomCommanded;
    else if (!strcmp(t[7], "UNKNOWN"))
      p.provenance = mulOdomUnknown;
    else
      goto syntax;

    if (!number(t[8], UINT32_MAX, &v))
      goto syntax;
    p.left = (uint32_t)v;
    if (!number(t[9], UINT32_MAX, &v))
      goto syntax;
    p.right = (uint32_t)v;
    if (!number(t[10], UINT32_MAX, &v))
      goto syntax;
    p.timeUncertaintyUs = (uint32_t)v;
    result(out, s, mulEkfStreamOdom(s->stream, &p));
    return 1;
  }
  if (n == 10 && !strcmp(t[1], "BEGIN")) {
    mulEkfBatch_t b = {0};
    uint64_t v;
    if (!number(t[2], UINT32_MAX, &v))
      goto syntax;
    b.clock = (uint32_t)v;
    if (!number(t[3], UINT32_MAX, &v))
      goto syntax;
    b.epoch = (uint32_t)v;
    if (!number(t[4], UINT32_MAX, &v))
      goto syntax;
    b.frame = (uint32_t)v;
    if (!number(t[5], UINT64_MAX, &b.group) || !number(t[6], UINT64_MAX, &b.timeUs) ||
        !number(t[7], MUL_EKF_MEASUREMENTS, &v))
      goto syntax;
    b.count = (uint8_t)v;
    if (!number(t[8], UINT32_MAX, &v))
      goto syntax;
    b.timeUncertaintyUs = (uint32_t)v;
    if (!number(t[9], 1, &v))
      goto syntax;
    b.independentPoseTimes = (uint8_t)v;
    if (!b.group || !b.count)
      goto syntax;

    s->assembly = b;
    s->assembling = true;
    s->rowMask = s->observationMask = 0;
    strcpy(out, "EKF1 batch=begun");
    return 1;
  }
  if (!s->assembling)
    goto syntax;

  if (n == 3 && !strcmp(t[1], "POSE")) {
    if (strlen(t[2]) != MUL_POSE_BYTES * 2 || s->assembly.anchorCount == MUL_POSE_MAX)
      goto syntax;

    uint8_t bytes[MUL_POSE_BYTES];
    for (unsigned i = 0; i < MUL_POSE_BYTES; i++) {
      unsigned value = 0;
      for (unsigned j = 0; j < 2; j++) {
        unsigned char ch = (unsigned char)t[2][2 * i + j];
        unsigned d = ch >= '0' && ch <= '9'   ? ch - '0'
                     : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                     : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                                              : 99;
        if (d > 15)
          goto syntax;
        value = value * 16 + d;
      }
      bytes[i] = (uint8_t)value;
    }
    mulPose_t p;
    if (!mulPoseDecode(bytes, receivedMs, &p) ||
        nodeIndex(&s->assembly, p.node) < s->assembly.anchorCount)
      goto syntax;

    s->assembly.anchors[s->assembly.anchorCount++] = p;
    strcpy(out, "EKF1 pose=accepted");
    return 1;
  }
  if (n == 10 && !strcmp(t[1], "OBS")) {
    uint64_t index, v;
    mulEkfMeasurement_t m = {0};
    uint8_t node, reference;
    if (!number(t[2], s->assembly.count - 1, &index) || (s->observationMask & (1u << index)) ||
        !number(t[3], UINT64_MAX, &m.id))
      goto syntax;

    if (!strcmp(t[4], "TDOA"))
      m.kind = MUL_EKF_TDOA;
    else if (!strcmp(t[4], "TWR"))
      m.kind = MUL_EKF_RANGE;
    else
      goto syntax;

    if (!number(t[5], 254, &v))
      goto syntax;
    node = (uint8_t)v;
    if (!number(t[6], 255, &v))
      goto syntax;
    reference = (uint8_t)v;
    if (!real(t[7], &m.valueM) || !number(t[8], 1, &v))
      goto syntax;
    m.valid = (uint8_t)v;
    if (!number(t[9], 1, &v))
      goto syntax;
    m.rateValid = (uint8_t)v;
    s->assembly.measurements[index] = m;
    s->nodes[index] = node;
    s->references[index] = reference;
    s->observationMask |= (uint8_t)(1u << index);
    strcpy(out, "EKF1 observation=accepted");
    return 1;
  }
  if (n == 3u + s->assembly.count && !strcmp(t[1], "COVROW")) {
    uint64_t row;
    if (!number(t[2], s->assembly.count - 1, &row) || (s->rowMask & (1u << row)))
      goto syntax;

    double values[MUL_EKF_MEASUREMENTS];
    for (unsigned i = 0; i < s->assembly.count; i++)
      if (!real(t[3 + i], &values[i]))
        goto syntax;

    memcpy(s->assembly.noise[row], values, s->assembly.count * sizeof(double));
    s->rowMask |= (uint8_t)(1u << row);
    strcpy(out, "EKF1 covariance_row=accepted");
    return 1;
  }
  if (n == 2 && !strcmp(t[1], "COMMIT")) {
    unsigned full = (1u << s->assembly.count) - 1;
    if (s->rowMask != full || s->observationMask != full)
      goto syntax;

    for (unsigned i = 0; i < s->assembly.count; i++) {
      unsigned node = nodeIndex(&s->assembly, s->nodes[i]),
               ref = nodeIndex(&s->assembly, s->references[i]);
      if (node == s->assembly.anchorCount ||
          (s->assembly.measurements[i].kind == MUL_EKF_TDOA && ref == s->assembly.anchorCount))
        goto syntax;

      s->assembly.measurements[i].anchor = (uint8_t)node;
      s->assembly.measurements[i].reference = (uint8_t)ref;
    }
    mulEkfStatus_t status = mulEkfStreamBatch(s->stream, &s->assembly);
    s->assembling = false;
    result(out, s, status);
    return 1;
  }
syntax:
  strcpy(out, "EKF ERR syntax/config/input; state retained; see EKF HELP");
  return 1;
}

int ekf_service_command(ekf_service_t *s, const char *input, uint32_t receivedMs,
                        char out[EKF_SERVICE_LINE]) {
  if (!out)
    return 0;
  memset(out, 0, EKF_SERVICE_LINE);
  int handled = command(s, input, receivedMs, out);
  // Bridge needs space for a newline; never expose truncated structured output.
  if (handled && strlen(out) >= EKF_SERVICE_LINE - 2)
    strcpy(out, "EKF ERR diagnostic overflow");
  return handled;
}
