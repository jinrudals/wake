/* Wake FUSE launcher to capture inputs/outputs
 *
 * Copyright 2019 SiFive, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You should have received a copy of LICENSE.Apache2 along with
 * this software. If not, you may obtain a copy at
 *
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <algorithm>
#include <iostream>
#include <fstream>

#include "json5.h"
#include "execpath.h"
#include "membytes.h"

#ifdef __linux__
// For unshare and bind mount
#include <sched.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/sysmacros.h>

static void write_file(const char *file, const char *content, int len)
{
	int fd;

	fd = open(file, O_WRONLY);
	if (fd < 0) {
		std::cerr << "open " << file << ": " << strerror(errno) << std::endl;
		exit(1);
	}

	if (write(fd, content, len) != len) {
		std::cerr << "write " << file << ": " << strerror(errno) << std::endl;
		exit(1);
	}

	close(fd);
}

static void map_id(const char *file, uint32_t from, uint32_t to)
{
	char buf[80];
	snprintf(buf, sizeof(buf), "%u %u 1", from, to);
	write_file(file, buf, strlen(buf));
}

// Inside a user namespace, you are not allowed to separate mounts that you inherit from another
// mount namespace from their child mounts.
// Therefore, when a mount has subdirectories containing other mounts we must be recursive when we bind mount.
static bool bind_mount(const std::string& source, const std::string& destination, bool readonly)
{
	const char* src = source.c_str();
	const char* dest = destination.c_str();

	if (0 != mount(src, dest, NULL, MS_BIND | MS_REC, NULL)) {
		std::cerr << "bind mount (" << source << " -> " << destination << "): "
			<< strerror(errno) << std::endl;
		return false;
	}
	// Re-mount to set destination as read-only.
	// Source filesystem must not have 'MS_NODEV' (a.k.a. 'nodev') set.
	if (readonly) {
		if (0 != mount(src, dest, NULL, MS_BIND | MS_REC | MS_RDONLY | MS_REMOUNT, NULL)) {
			std::cerr << "bind mount (" << source << " -> " << destination << "): "
				<< strerror(errno) << std::endl;
			return false;
		}
	}
	return true;
}

static bool validate_mount(
	const std::string& op,
	const std::string& source,
	const std::string& after_pivot)
{
	static const std::vector<std::string> mount_ops {
		// must be sorted
		"bind",
		"pivot-root",
		"squashfs",
		"tmpfs",
		"workspace"
	};

	if (!std::binary_search(mount_ops.begin(), mount_ops.end(), op)) {
		std::cerr << "unknown mount type: '" << op << "'" << std::endl;
		return false;
	}

	if ((op != "bind" && op != "squashfs") && source.length() != 0) {
		std::cerr << "mount: " << op << " can not have 'source' option" << std::endl;
		return false;
	}
	if (op != "workspace" && after_pivot.length() != 0) {
		std::cerr << "mount: " << op << " can not have 'after-pivot' option" << std::endl;
		return false;
	}

	return true;
}

// The pivot_root syscall has no glibc wrapper.
static int pivot_root(const char* new_root, const char* put_old) {
#ifndef __NR_pivot_root
#error 'pivot_root' syscall number missing from <sys/syscall.h>
#endif
	return syscall(__NR_pivot_root, new_root, put_old);
}

/* Many systems have an ancient manpage entry for pivot_root,
 * See 2019-era docs at: https://lwn.net/Articles/800381/
 *
 * new_root and put_old may be the same directory.
 * In particular, the following sequence allows a pivot-root operation without needing
 * to create and remove a temporary directory:
 *
 *   chdir(new_root);
 *   pivot_root(".", ".");
 *   umount2(".", MNT_DETACH);
 *
 * This sequence succeeds because the pivot_root() call stacks the old root mount point
 * on top of the new root mount point at /. At that point, the calling process's root
 * directory and current working directory refer to the new root mount point (new_root).
 * During the subsequent umount() call, resolution of "." starts with new_root and then
 * moves up the list of mounts stacked at /, with the result that old root mount point
 * is unmounted.
 */
static bool do_pivot(const std::string& newroot) {
	// The pivot_root syscall requires that the new root location is a mountpoint.
	// Bind mount the new root onto itself to ensure this.
	if (!bind_mount(newroot, newroot, false)) {
		return false;
	}
	if (0 != chdir(newroot.c_str())) {
		std::cerr << "chdir: " << strerror(errno) << std::endl;
		return false;
	}
	if (0 != pivot_root(".", ".")) {
		std::cerr << "pivot_root(\".\", \".\"): " << strerror(errno) << std::endl;
		return false;
	}
	if (0 != umount2(".", MNT_DETACH)) {
		std::cerr << "umount2: " << strerror(errno) << std::endl;
		return false;
	}
	return true;
}

static bool mount_tmpfs(const std::string& destination) {
	if (0 != mount("tmpfs", destination.c_str(), "tmpfs", 0UL, NULL)) {
		std::cerr << "tmpfs mount (" << destination << "): "
			<< strerror(errno) << std::endl;
		return false;
	}
	return true;
}

static bool equal_dev_ids(dev_t a, dev_t b) {
	return major(a) == major(b) && minor(a) == minor(b);
}

static bool mount_squashfs(const std::string& source, const std::string& mountpoint) {
	pid_t pid = fork();
	if (pid == 0) {
		// kernel to send SIGKILL to squashfuse when fuse-wake terminates
		if (prctl(PR_SET_PDEATHSIG, SIGKILL) == -1) {
			std::cerr << "squashfuse prctl: " << strerror(errno) << std::endl;
			exit(1);
		}
		execlp("squashfuse", "squashfuse", "-f", source.c_str(), mountpoint.c_str(), NULL);
		std::cerr << "execlp squashfuse: " << strerror(errno) << std::endl;
		exit(1);
	}

	// Wait for the mount to exist before we continue by checking if the
	// stat() device id or the inode changes.
	struct stat before;
	if (0 != stat(mountpoint.c_str(), &before)) {
		std::cerr << "stat (" << mountpoint << "): " << strerror(errno) << std::endl;
		return false;
	}

	for (int i = 0; i < 10; i++) {
		struct stat after;
		if (0 != stat(mountpoint.c_str(), &after)) {
			std::cerr << "stat (" << mountpoint << "): " << strerror(errno) << std::endl;
			return false;
		}

		if (!equal_dev_ids(before.st_dev, after.st_dev) || (before.st_ino != after.st_ino))
			return true;
		else
			usleep(10000 << i); // 10ms * 2^i
	}

	std::cerr << "squashfs mount missing: " << mountpoint << std::endl;
	return false;
}

// Do the mounts specified in the parsed input json.
// The input/caller responsibility to ensure that the mountpoint exists,
// that the platform supports the mount type/options, and to correctly order
// the layered mounts.
static bool do_mounts_from_json(const JAST& jast, const std::string& fuse_mount_path)
{
	for (auto &x : jast.get("mounts").children) {
		const std::string& op = x.second.get("type").value;
		const std::string& src = x.second.get("source").value;
		const std::string& dest = x.second.get("destination").value;
		const std::string& after_pivot = x.second.get("after-pivot").value;
		bool readonly = x.second.get("read-only").kind == JSON_TRUE;

		if (!validate_mount(op, src, after_pivot))
			return false;

		if (op == "bind" && !bind_mount(src, dest, readonly))
			return false;

		if (op == "workspace" && !bind_mount(fuse_mount_path, dest, false))
			return false;

		if (op == "pivot-root" && !do_pivot(dest))
			return false;

		if (op == "tmpfs" && !mount_tmpfs(dest))
			return false;

		if (op == "squashfs" && !mount_squashfs(src, dest))
			return false;

	}
	return true;
}

static bool get_workspace_dir(
	const JAST& jast,
	const std::string& host_workspace_dir,
	std::string& out)
{
	for (auto &x : jast.get("mounts").children) {
		const std::string& op = x.second.get("type").value;
		if (op == "workspace") {
			std::string after_pivot = x.second.get("after-pivot").value;
			if (after_pivot.length() > 0) {
				out = after_pivot;
			} else {
				out = x.second.get("destination").value;
				// convert a workspace relative path into absolute path
				if (out.at(0) != '/')
					out = host_workspace_dir + "/" + out;
			}
			return true;
		}
	}
	return false;
}

static bool setup_user_namespaces(const JAST& jast) {
	uid_t real_euid = geteuid();
	gid_t real_egid = getegid();

	uid_t euid = real_euid;
	gid_t egid = real_egid;
	int flags = CLONE_NEWNS|CLONE_NEWUSER;

	for (auto &res : jast.get("resources").children) {
		const std::string &key = res.second.value;
		if (key == "isolate/user") euid = egid = 0;
		if (key == "isolate/host") flags |= CLONE_NEWUTS;
		if (key == "isolate/net") flags |= CLONE_NEWNET;
	}

	// Enter a new mount namespace we can control
	if (0 != unshare(flags)) {
		std::cerr << "unshare: " << strerror(errno) << std::endl;
		return false;
	}

	// Wipe out our hostname
	if (0 != (flags & CLONE_NEWUTS)) {
		if (sethostname("build", 5) != 0) {
			std::cerr << "sethostname(build): " << strerror(errno) << std::endl;
			return false;
		}
		if (setdomainname("local", 5) != 0) {
			std::cerr << "setdomainname(local): " << strerror(errno) << std::endl;
			return false;
		}
	}

	// Map our UID to either our original UID or root
	write_file("/proc/self/setgroups", "deny", 4);
	map_id("/proc/self/uid_map", euid, real_euid);
	map_id("/proc/self/gid_map", egid, real_egid);

	return true;
}

#endif

int main(int argc, char *argv[])
{
	if (argc != 3) {
		std::cerr << "Syntax: fuse-wake <input-json> <output-json>" << std::endl;
		return 1;
	}

	std::ifstream ifs(argv[1]);
	std::string json(
		(std::istreambuf_iterator<char>(ifs)),
		(std::istreambuf_iterator<char>()));
	if (ifs.fail()) {
		std::cerr << "read " << argv[1] << ": " << strerror(errno) << std::endl;
		return 1;
	}
	ifs.close();

	std::ofstream ofs(argv[2], std::ios_base::trunc);
	if (ofs.fail()) {
		std::cerr << "write " << argv[2] << ": " << strerror(errno) << std::endl;
		return 1;
	}

	JAST jast;
	if (!JAST::parse(json, std::cerr, jast))
		return 1;

	std::string exedir = find_execpath();
	std::string daemon = exedir + "/fuse-waked";
	std::string name  = std::to_string(getpid());
	std::string cwd   = get_cwd();
	// mpath is where the fuse filesystem is mounted
	std::string mpath = cwd + "/.fuse";
	std::string fpath = mpath + "/.f.fuse-waked";
	// rpath is a subdir in the fuse filesystem that will be used by this fuse-wake
	std::string rpath = mpath + "/" + name;
	std::string lpath = mpath + "/.l." + name;
	std::string ipath = mpath + "/.i." + name;
	std::string opath = mpath + "/.o." + name;

	int ffd = -1;
	useconds_t wait = 10000; /* 10ms */
	for (int retry = 0; (ffd = open(fpath.c_str(), O_RDONLY)) == -1 && retry < 12; ++retry) {
		pid_t pid = fork();
		if (pid == 0) {
			ofs.close();
			const char *env[2] = { "PATH=/usr/bin:/bin:/usr/sbin:/sbin", 0 };
			execle(daemon.c_str(), "fuse-waked", mpath.c_str(), nullptr, env);
			std::cerr << "execl " << daemon << ": " << strerror(errno) << std::endl;
			exit(1);
		}
		usleep(wait);
		wait <<= 1;

		int status;
		do waitpid(pid, &status, 0);
		while (WIFSTOPPED(status));
	}

	if (ffd == -1) {
		std::cerr << "Could not contact FUSE daemon" << std::endl;
		return 1;
	}

	// This stays open (keeping rpath live) until we terminate
	// Note: O_CLOEXEC is NOT set; thus, children keep rpath live as well
	int livefd = open(lpath.c_str(), O_CREAT|O_RDWR|O_EXCL, 0644);
	if (livefd == -1) {
		std::cerr << "open " << rpath << ": " << strerror(errno) << std::endl;
		return 1;
	}

	// We can safely release the global handle now that we hold a livefd
	(void)close(ffd);

	std::ofstream ijson(ipath);
	ijson.write(json.data(), json.size());
	if (ijson.fail()) {
		std::cerr << "write " << ipath << ": " << strerror(errno) << std::endl;
		return 1;
	}
	ijson.close();
	json.clear();

	struct timeval start;
	gettimeofday(&start, 0);

	pid_t pid = fork();
	if (pid == 0) {
		ofs.close();
		// Become a target for group death
		// setpgid(0, 0);

		// Prepare the subcommand inputs
		std::vector<char *> arg, env;
		for (auto &x : jast.get("command").children)
			arg.push_back(const_cast<char*>(x.second.value.c_str()));
		arg.push_back(0);
		for (auto &x : jast.get("environment").children)
			env.push_back(const_cast<char*>(x.second.value.c_str()));
		env.push_back(0);

		std::string subdir = jast.get("directory").value;
		std::string stdin = jast.get("stdin").value;
		if (stdin.empty()) stdin = "/dev/null";

#ifdef __linux__

		if (!setup_user_namespaces(jast))
			exit(1);

		// Mounts from the parsed input json.
		if (!do_mounts_from_json(jast, rpath))
			exit(1);

		std::string dir;
		if (!get_workspace_dir(jast, cwd, dir)) {
			std::cerr << "'workspace' mount entry is missing from input" << std::endl;
			exit(1);
		}
		dir = dir + "/" + subdir;
#else
		std::string dir = rpath + "/" + subdir;
#endif

		if (chdir(dir.c_str()) != 0) {
			std::cerr << "chdir " << dir << ": " << strerror(errno) << std::endl;
			exit(1);
		}

		int fd = open(stdin.c_str(), O_RDONLY);
		if (fd == -1) {
			std::cerr << "open " << stdin << ":" << strerror(errno) << std::endl;
			exit(1);
		}

		if (fd != STDIN_FILENO) {
			dup2(fd, STDIN_FILENO);
			close(fd);
		}

		std::string command = find_in_path(arg[0], find_path(env.data()));
		execve(command.c_str(), arg.data(), env.data());
		std::cerr << "execve " << command << ": " << strerror(errno) << std::endl;
		exit(1);
	}
	jast.children.clear();

	// Don't hold IO open while waiting
	(void)close(STDIN_FILENO);
	(void)close(STDOUT_FILENO);
	(void)close(STDERR_FILENO);

	int status;
	struct rusage rusage;
	do wait4(pid, &status, 0, &rusage);
	while (WIFSTOPPED(status));

	if (WIFEXITED(status)) {
		status = WEXITSTATUS(status);
	} else {
		status = -WTERMSIG(status);
	}

	struct timeval stop;
	gettimeofday(&stop, 0);

	// Cause the opath to be generated (this write will fail)
	(void)!write(livefd, &stop, 1); // the ! convinces older gcc that it's ok to ignore the write
	(void)fsync(livefd);

	if (!JAST::parse(opath.c_str(), ofs, jast))
		return 1;

	bool first;
	ofs << "{\"usage\":{\"status\":" << status
	  << ",\"runtime\":" << (stop.tv_sec - start.tv_sec + (stop.tv_usec - start.tv_usec)/1000000.0)
	  << ",\"cputime\":" << (rusage.ru_utime.tv_sec + rusage.ru_stime.tv_sec + (rusage.ru_utime.tv_usec + rusage.ru_stime.tv_usec)/1000000.0)
	  << ",\"membytes\":" << MEMBYTES(rusage)
	  << ",\"inbytes\":" << jast.get("ibytes").value
	  << ",\"outbytes\":" << jast.get("obytes").value
	  << "},\"inputs\":[";

	first = true;
	for (auto &x : jast.get("inputs").children) {
		ofs << (first?"":",") << "\"" << json_escape(x.second.value) << "\"";
		first = false;
	}

	ofs << "],\"outputs\":[";

	first = true;
	for (auto &x : jast.get("outputs").children) {
		ofs << (first?"":",") << "\"" << json_escape(x.second.value) << "\"";
		first = false;
	}

	ofs << "]}" << std::endl;

	return ofs.fail() ? 1 : 0;
}
