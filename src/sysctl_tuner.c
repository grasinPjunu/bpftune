#include <libbpftune.h>
#include "sysctl_tuner.skel.h"

#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

int init(struct bpftuner *tuner, int ringbuf_map_fd)
{
	struct sysctl_tuner_bpf *skel;
	int prog_fd, cgroup_fd, err;
	const char *cgroup_dir;

	bpftuner_bpf_init(sysctl, tuner, ringbuf_map_fd);

	/* attach to root cgroup */
	cgroup_dir = bpftune_cgroup_name();

	if (!cgroup_dir) {
		bpftune_log(LOG_ERR, "cannot get cgroup_dir\n");
		return 1;
	}
	cgroup_fd = bpftune_cgroup_fd();
	skel = tuner->skel;
	prog_fd = bpf_program__fd(skel->progs.sysctl_write);

	if (bpf_prog_attach(prog_fd, cgroup_fd,
			    BPF_CGROUP_SYSCTL, BPF_F_ALLOW_MULTI)) {
		err = -errno;
		bpftune_log(LOG_ERR, "cannot attach to cgroup '%s': %s\n",
			    cgroup_dir, strerror(-err));
		return 1;
	}
	bpftune_log(LOG_DEBUG, "attached prog fd %d to cgroup fd %d\n",
		    prog_fd, cgroup_fd);
	/* set our pid so we can identify tuning events that come from
	 * outside.
	 */
	skel->bss->bpftune_pid = getpid();

	return 0;
}

void fini(struct bpftuner *tuner)
{
	struct sysctl_tuner_bpf *skel;
	int err, prog_fd, cgroup_fd;

	skel = tuner->skel;

	bpftune_log(LOG_DEBUG, "calling fini for %s\n", tuner->name);
	if (skel->progs.sysctl_write) {
		prog_fd = bpf_program__fd(skel->progs.sysctl_write);
		cgroup_fd = bpftune_cgroup_fd();

		if (bpf_prog_detach2(prog_fd, cgroup_fd, BPF_CGROUP_SYSCTL)) {
			err = -errno;
			bpftune_log(LOG_ERR, "error detaching prog fd %d, cgroup fd %d: %s\n",
				prog_fd, cgroup_fd, strerror(-err));
		}
	}
	bpftuner_bpf_fini(tuner);
}

void event_handler(struct bpftuner *tuner, struct bpftune_event *event,
		   __attribute__((unused))void *ctx)
{
	struct bpftuner *t;

	bpftune_log(LOG_DEBUG, "sysctl write for '%s' (scenario %d) for tuner %s\n",
		    event->str, event->scenario_id, tuner->name);

	bpftune_for_each_tuner(t) {
		struct bpftunable *tunable;

		bpftuner_for_each_tunable(t, tunable) {
			char path[512];

			bpftune_sysctl_name_to_path(tunable->desc.name, path,
						    sizeof(path));

			if (strstr(path, event->str)) {
				bpftune_log(LOG_INFO,
					    "user modified sysctl '%s' that tuner '%s' uses; disabling '%s'!\n",
					    event->str, t->name, t->name);
				bpftuner_fini(tuner, BPFTUNE_MANUAL);
				break;
			}
		}
	}
}
