#include <arch/arch.h>
#include <task/task.h>
#include <drivers/kernel_logger.h>
#include <fs/vfs/vfs.h>
#include <arch/arch.h>
#include <mm/mm.h>
#include <fs/fs_syscall.h>
#include <net/socket.h>

task_t *tasks[MAX_TASK_NUM];
task_t *idle_tasks[MAX_CPU_NUM];

bool task_initialized = false;
bool can_schedule = false;

uint64_t jiffies = 0;

extern int unix_socket_fsid;
extern int unix_accept_fsid;

task_t *get_free_task()
{
    for (uint64_t i = 0; i < cpu_count; i++)
    {
        if (idle_tasks[i] == NULL)
        {
            idle_tasks[i] = (task_t *)malloc(sizeof(task_t));
            memset(idle_tasks[i], 0, sizeof(task_t));
            idle_tasks[i]->pid = 0;
            return idle_tasks[i];
        }
    }

    for (uint64_t i = 1; i < MAX_TASK_NUM; i++)
    {
        if (tasks[i] == NULL)
        {
            tasks[i] = (task_t *)malloc(sizeof(task_t));
            memset(tasks[i], 0, sizeof(task_t));
            tasks[i]->pid = i;
            return tasks[i];
        }
    }

    return NULL;
}

uint32_t cpu_idx = 0;

uint32_t alloc_cpu_id()
{
    uint32_t idx = cpu_idx;
    cpu_idx = (cpu_idx + 1) % cpu_count;
    return idx;
}

task_t *task_create(const char *name, void (*entry)(uint64_t), uint64_t arg)
{
    arch_disable_interrupt();

    can_schedule = false;

    task_t *task = get_free_task();
    task->cpu_id = alloc_cpu_id();
    task->ppid = task->pid;
    task->uid = 0;
    task->gid = 0;
    task->euid = 0;
    task->egid = 0;
    task->pgid = 0;
    task->waitpid = 0;
    task->state = TASK_READY;
    task->current_state = TASK_READY;
    task->jiffies = 0;
    task->kernel_stack = (uint64_t)alloc_frames_bytes(STACK_SIZE) + STACK_SIZE;
    task->syscall_stack = (uint64_t)alloc_frames_bytes(STACK_SIZE) + STACK_SIZE;
    memset((void *)(task->kernel_stack - STACK_SIZE), 0, STACK_SIZE);
    memset((void *)(task->syscall_stack - STACK_SIZE), 0, STACK_SIZE);
    task->arch_context = malloc(sizeof(arch_context_t));
    memset(task->arch_context, 0, sizeof(arch_context_t));
    arch_context_init(task->arch_context, virt_to_phys((uint64_t)get_kernel_page_dir()), (uint64_t)entry, task->kernel_stack, false, arg);
    task->signal = 0;
    task->status = 0;
    task->cwd = rootdir;
    task->mmap_start = USER_MMAP_START;
    task->brk_start = USER_BRK_START;
    task->brk_end = USER_BRK_START;
    memset(task->actions, 0, sizeof(task->actions));
    memset(task->fds, 0, sizeof(task->fds));
    task->fds[0] = malloc(sizeof(fd_t));
    task->fds[0]->node = vfs_open("/dev/stdin");
    task->fds[0]->offset = 0;
    task->fds[0]->flags = 0;
    task->fds[1] = malloc(sizeof(fd_t));
    task->fds[1]->node = vfs_open("/dev/stdout");
    task->fds[1]->offset = 0;
    task->fds[1]->flags = 0;
    task->fds[2] = malloc(sizeof(fd_t));
    task->fds[2]->node = vfs_open("/dev/stderr");
    task->fds[2]->offset = 0;
    task->fds[2]->flags = 0;
    strncpy(task->name, name, TASK_NAME_MAX);

    memset(&task->term, 0, sizeof(termios));
    task->term.c_iflag = BRKINT | ICRNL | INPCK | ISTRIP | IXON;
    task->term.c_oflag = OPOST;
    task->term.c_cflag = CS8 | CREAD | CLOCAL;
    task->term.c_lflag = ECHO | ICANON | IEXTEN | ISIG;
    task->term.c_line = 0;
    task->term.c_cc[VINTR] = 3;     // Ctrl-C
    task->term.c_cc[VQUIT] = 28;    // Ctrl-task->term.c_cc[VERASE] = 127; // DEL
    task->term.c_cc[VKILL] = 21;    // Ctrl-U
    task->term.c_cc[VEOF] = 4;      // Ctrl-D
    task->term.c_cc[VTIME] = 0;     // No timer
    task->term.c_cc[VMIN] = 1;      // Return each byte
    task->term.c_cc[VSTART] = 17;   // Ctrl-Q
    task->term.c_cc[VSTOP] = 19;    // Ctrl-S
    task->term.c_cc[VSUSP] = 26;    // Ctrl-Z
    task->term.c_cc[VREPRINT] = 18; // Ctrl-R
    task->term.c_cc[VDISCARD] = 15; // Ctrl-O
    task->term.c_cc[VWERASE] = 23;  // Ctrl-W
    task->term.c_cc[VLNEXT] = 22;   // Ctrl-V
    // Initialize other control characters to 0
    for (int i = 16; i < NCCS; i++)
    {
        task->term.c_cc[i] = 0;
    }

    task->tmp_rec_v = 0;
    task->cmdline = NULL;

    memset(task->actions, 0, sizeof(task->actions));

    memset(task->rlim, 0, sizeof(task->rlim));
    task->rlim[RLIMIT_NPROC] = (struct rlimit){0, MAX_TASK_NUM};
    task->rlim[RLIMIT_NOFILE] = (struct rlimit){MAX_FD_NUM, MAX_FD_NUM};
    task->rlim[RLIMIT_CORE] = (struct rlimit){0, 0};

    socket_on_new_task(task->pid);

    can_schedule = true;

    return task;
}

task_t *task_search(task_state_t state, uint32_t cpu_id)
{
    task_t *task = NULL;

    for (size_t i = 1; i < MAX_TASK_NUM; i++)
    {
        task_t *ptr = tasks[i];
        if (ptr == NULL)
            break;
        if (ptr->state != state)
            continue;
        if (current_task == ptr)
            continue;
        if (ptr->cpu_id != cpu_id)
            continue;

        if (task == NULL || ptr->jiffies < task->jiffies)
            task = ptr;
    }

    if (task == NULL && state == TASK_READY)
    {
        task = idle_tasks[cpu_id];
    }

    return task;
}

void idle_entry(uint64_t arg)
{
    while (1)
    {
        arch_enable_interrupt();
        arch_pause();
    }
}

#include <drivers/bus/pci.h>
#include <drivers/block/ahci/ahci.h>
#include <drivers/block/nvme/nvme.h>
#include <drivers/usb/usb.h>
#include <drivers/drm/drm.h>
#include <drivers/virtio/virtio.h>
#include <drivers/net/net.h>
#include <fs/partition.h>
#include <drivers/fb.h>

extern void ext2_init();
extern void fatfs_init();
extern void iso9660_init();
extern void sysfs_init();
extern void fs_syscall_init();
extern void pipefs_init();
extern void socketfs_init();

extern void mount_root();

bool system_initialized = false;

void init_thread(uint64_t arg)
{
    printk("NAOS init thread is running...\n");

    pci_init();
#if defined(__x86_64__)
    ahci_init();
#endif
    nvme_init();

    virtio_init();

    usb_init();

    drm_init();

    partition_init();
    fbdev_init();

    sysfs_init();

    fbdev_init_sysfs();
    drm_init_sysfs();

    net_init();

    fs_syscall_init();
    socketfs_init();
    pipefs_init();
    ext2_init();
    iso9660_init();
    fatfs_init();

    mount_root();

    arch_input_dev_init();

    system_initialized = true;

    task_execve("/bin/bash", NULL, NULL);

    printk("run /bin/bash failed\n");

    while (1)
    {
        arch_pause();
    }
}

void task_init()
{
    memset(tasks, 0, sizeof(tasks));
    memset(idle_tasks, 0, sizeof(idle_tasks));

    for (uint64_t cpu = 0; cpu < cpu_count; cpu++)
    {
        idle_tasks[cpu] = task_create("idle", idle_entry, 0);
        idle_tasks[cpu]->cpu_id = cpu;
        idle_tasks[cpu]->state = TASK_RUNNING;
    }
    arch_set_current(idle_tasks[0]);
    task_create("init", init_thread, 0);

    task_initialized = true;

    can_schedule = true;
}

uint64_t push_slice(uint64_t ustack, uint8_t *slice, uint64_t len)
{
    uint64_t tmp_stack = ustack;
    tmp_stack -= len;
    tmp_stack -= (tmp_stack % 0x08);

    memcpy((void *)tmp_stack, slice, len);

    return tmp_stack;
}

uint64_t push_infos(task_t *task, uint64_t current_stack, char *argv[], char *envp[], uint64_t e_entry, uint64_t phdr, uint64_t phnum, uint64_t at_base)
{
    uint64_t env_i = 0;
    uint64_t argv_i = 0;

    uint64_t tmp_stack = current_stack;
    tmp_stack = push_slice(tmp_stack, (uint8_t *)task->name, strlen(task->name) + 1);

    uint64_t execfn_ptr = tmp_stack;

    uint64_t *envps = (uint64_t *)malloc(1024);
    memset(envps, 0, 1024);
    uint64_t *argvps = (uint64_t *)malloc(1024);
    memset(argvps, 0, 1024);

    if (envp != NULL)
    {
        // push envs
        for (env_i = 0; envp[env_i] != NULL; env_i++)
        {
            tmp_stack = push_slice(tmp_stack, (uint8_t *)envp[env_i], strlen(envp[env_i]) + 1);
            envps[env_i] = tmp_stack;
        }
    }

    if (argv != NULL)
    {
        // push argvs
        for (argv_i = 0; argv[argv_i] != NULL; argv_i++)
        {
            tmp_stack = push_slice(tmp_stack, (uint8_t *)argv[argv_i], strlen(argv[argv_i]) + 1);
            argvps[argv_i] = tmp_stack;
        }
    }

    uint64_t total_length = 2 * sizeof(uint64_t) + 7 * 2 * sizeof(uint64_t) + (env_i + 0) * sizeof(uint64_t) + sizeof(uint64_t) + (argv_i + 0) * sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint64_t);
    tmp_stack -= (tmp_stack - total_length) % 0x10;

    // push auxv
    uint8_t *tmp = (uint8_t *)malloc(2 * sizeof(uint64_t));
    memset(tmp, 0, 2 * sizeof(uint64_t));
    tmp_stack = push_slice(tmp_stack, tmp, 2 * sizeof(uint64_t));

    ((uint64_t *)tmp)[0] = AT_PHDR;
    ((uint64_t *)tmp)[1] = phdr;
    tmp_stack = push_slice(tmp_stack, tmp, 2 * sizeof(uint64_t));

    ((uint64_t *)tmp)[0] = AT_PHENT;
    ((uint64_t *)tmp)[1] = sizeof(Elf64_Phdr);
    tmp_stack = push_slice(tmp_stack, tmp, 2 * sizeof(uint64_t));

    ((uint64_t *)tmp)[0] = AT_PHNUM;
    ((uint64_t *)tmp)[1] = phnum;
    tmp_stack = push_slice(tmp_stack, tmp, 2 * sizeof(uint64_t));

    ((uint64_t *)tmp)[0] = AT_ENTRY;
    ((uint64_t *)tmp)[1] = e_entry;
    tmp_stack = push_slice(tmp_stack, tmp, 2 * sizeof(uint64_t));

    ((uint64_t *)tmp)[0] = AT_EXECFN;
    ((uint64_t *)tmp)[1] = execfn_ptr;
    tmp_stack = push_slice(tmp_stack, tmp, 2 * sizeof(uint64_t));

    ((uint64_t *)tmp)[0] = AT_BASE;
    ((uint64_t *)tmp)[1] = at_base;
    tmp_stack = push_slice(tmp_stack, tmp, 2 * sizeof(uint64_t));

    ((uint64_t *)tmp)[0] = AT_PAGESZ;
    ((uint64_t *)tmp)[1] = DEFAULT_PAGE_SIZE;
    tmp_stack = push_slice(tmp_stack, tmp, 2 * sizeof(uint64_t));

    memset(tmp, 0, 2 * sizeof(uint64_t));

    // push envp
    tmp_stack = push_slice(tmp_stack, tmp, sizeof(uint64_t));
    tmp_stack = push_slice(tmp_stack, (uint8_t *)envps, env_i * sizeof(uint64_t));

    // push argvp
    tmp_stack = push_slice(tmp_stack, tmp, sizeof(uint64_t));
    tmp_stack = push_slice(tmp_stack, (uint8_t *)argvps, argv_i * sizeof(uint64_t));

    tmp_stack = push_slice(tmp_stack, (uint8_t *)&argv_i, sizeof(uint64_t));

    free(tmp);
    free(envps);
    free(argvps);

    return tmp_stack;
}

uint64_t task_fork(struct pt_regs *regs, bool vfork)
{
    arch_disable_interrupt();

    can_schedule = false;

    task_t *child = get_free_task();
    if (child == NULL)
    {
        can_schedule = true;
        return (uint64_t)-ENOMEM;
    }

    strncpy(child->name, current_task->name, TASK_NAME_MAX);

    child->state = TASK_READY;
    child->current_state = TASK_READY;

    child->cpu_id = alloc_cpu_id();

    child->kernel_stack = (uint64_t)alloc_frames_bytes(STACK_SIZE) + STACK_SIZE;
    child->syscall_stack = (uint64_t)alloc_frames_bytes(STACK_SIZE) + STACK_SIZE;
    memset((void *)(child->kernel_stack - STACK_SIZE), 0, STACK_SIZE);
    memset((void *)(child->syscall_stack - STACK_SIZE), 0, STACK_SIZE);

    child->arch_context = malloc(sizeof(arch_context_t));
    memset(child->arch_context, 0, sizeof(arch_context_t));
    current_task->arch_context->ctx = regs;
    arch_context_copy(child->arch_context, current_task->arch_context, child->kernel_stack, vfork ? CLONE_VM : 0);
    child->ppid = current_task->pid;
    child->uid = current_task->uid;
    child->gid = current_task->gid;
    child->euid = current_task->euid;
    child->egid = current_task->egid;
    child->pgid = current_task->pgid;

    child->jiffies = current_task->jiffies;

    child->cwd = current_task->cwd;
    child->cmdline = current_task->cmdline;

    child->mmap_start = USER_MMAP_START;
    child->brk_start = USER_BRK_START;
    child->brk_end = USER_BRK_START;
    child->load_start = current_task->load_start;
    child->load_end = current_task->load_end;

    memset(child->fds, 0, sizeof(child->fds));
    child->fds[0] = malloc(sizeof(fd_t));
    child->fds[0]->node = vfs_open("/dev/stdin");
    child->fds[0]->offset = 0;
    child->fds[0]->flags = 0;
    child->fds[1] = malloc(sizeof(fd_t));
    child->fds[1]->node = vfs_open("/dev/stdout");
    child->fds[1]->offset = 0;
    child->fds[1]->flags = 0;
    child->fds[2] = malloc(sizeof(fd_t));
    child->fds[2]->node = vfs_open("/dev/stderr");
    child->fds[2]->offset = 0;
    child->fds[2]->flags = 0;

    if (!vfork)
    {
        for (uint64_t i = 3; i < MAX_FD_NUM; i++)
        {
            fd_t *fd = current_task->fds[i];

            if (fd)
            {
                child->fds[i] = vfs_dup(fd);
            }
            else
            {
                child->fds[i] = NULL;
            }
        }
    }

    memcpy(child->actions, current_task->actions, sizeof(child->actions));
    child->signal = current_task->signal;
    child->blocked = current_task->blocked;

    memcpy(&child->term, &current_task->term, sizeof(termios));

    child->tmp_rec_v = current_task->tmp_rec_v;

    memcpy(child->rlim, current_task->rlim, sizeof(child->rlim));

    socket_on_new_task(child->pid);

    can_schedule = true;

    return child->pid;
}

bool execve_lock = false;

uint64_t task_execve(const char *path, const char **argv, const char **envp)
{
    while (execve_lock)
    {
        arch_enable_interrupt();

        arch_pause();
    }

    arch_disable_interrupt();

    can_schedule = false;

    execve_lock = true;

    vfs_node_t node = vfs_open(path);
    if (!node)
    {
        can_schedule = true;
        execve_lock = false;
        return (uint64_t)-ENOENT;
    }

    uint64_t buf_len = (node->size + DEFAULT_PAGE_SIZE - 1) & (~(DEFAULT_PAGE_SIZE - 1));

    char **new_argv = (char **)malloc(1024);
    memset(new_argv, 0, 1024);
    char **new_envp = (char **)malloc(1024);
    memset(new_envp, 0, 1024);

    int argv_count = 0;
    int envp_count = 0;

    if (argv && (translate_address(get_current_page_dir(true), (uint64_t)argv) != 0))
    {
        for (argv_count = 0; argv[argv_count] != NULL && (translate_address(get_current_page_dir(true), (uint64_t)argv[argv_count]) != 0); argv_count++)
        {
            new_argv[argv_count] = strdup(argv[argv_count]);
        }
    }
    new_argv[argv_count] = NULL;

    if (envp && (translate_address(get_current_page_dir(true), (uint64_t)envp) != 0))
    {
        for (envp_count = 0; envp[envp_count] != NULL && (translate_address(get_current_page_dir(true), (uint64_t)envp[envp_count]) != 0); envp_count++)
        {
            new_envp[envp_count] = strdup(envp[envp_count]);
        }
    }
    new_envp[envp_count] = NULL;

#if defined(__x86_64__)
    if (current_task->arch_context->mm->page_table_addr == (uint64_t)virt_to_phys(get_kernel_page_dir()))
    {
        current_task->arch_context->mm = clone_page_table(current_task->arch_context->mm, CLONE_VM);
        asm volatile("movq %0, %%cr3" ::"r"(current_task->arch_context->mm->page_table_addr));
    }
#endif

    uint8_t *buffer = (uint8_t *)EHDR_START_ADDR;
    map_page_range(get_current_page_dir(true), EHDR_START_ADDR, 0, buf_len, PT_FLAG_R | PT_FLAG_W | PT_FLAG_U);

    vfs_read(node, buffer, 0, node->size);

    char *fullpath = vfs_get_fullpath(node);

    vfs_close(node);

    if (buffer[0] == '#' && buffer[1] == '!')
    {
        for (int i = 0; i < argv_count; i++)
            if (new_argv[i])
                free(new_argv[i]);
        free(new_argv);
        for (int i = 0; i < envp_count; i++)
            if (new_envp[i])
                free(new_envp[i]);
        free(new_envp);

        execve_lock = false;
        const char *argvs[64];
        memset(argvs, 0, 64 * sizeof(const char *));
        argvs[0] = "/bin/sh";
        argvs[1] = path;
        int i;
        for (i = 0; i < argv_count; i++)
            argvs[i + 2] = argv[i];
        argvs[i] = NULL;
        return task_execve("/bin/sh", argvs, envp);
    }

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)EHDR_START_ADDR;

    uint64_t e_entry = ehdr->e_entry;

    uint64_t interpreter_entry = 0;

    if (e_entry == 0)
    {
        free(fullpath);
        for (int i = 0; i < argv_count; i++)
            if (new_argv[i])
                free(new_argv[i]);
        free(new_argv);
        for (int i = 0; i < envp_count; i++)
            if (new_envp[i])
                free(new_envp[i]);
        free(new_envp);
        can_schedule = true;
        execve_lock = false;
        return (uint64_t)-EINVAL;
    }

    if (!arch_check_elf(ehdr))
    {
        free(fullpath);
        for (int i = 0; i < argv_count; i++)
            if (new_argv[i])
                free(new_argv[i]);
        free(new_argv);
        for (int i = 0; i < envp_count; i++)
            if (new_envp[i])
                free(new_envp[i]);
        free(new_envp);
        can_schedule = true;
        execve_lock = false;
        return (uint64_t)-EINVAL;
    }

    // 处理程序头
    Elf64_Phdr *phdr = (Elf64_Phdr *)(EHDR_START_ADDR + ehdr->e_phoff);

    uint64_t load_start = UINT64_MAX;
    uint64_t load_end = 0;

    for (int i = 0; i < ehdr->e_phnum; ++i)
    {
        if (phdr[i].p_type == PT_INTERP)
        {
            const char *interpreter_name = ((const char *)ehdr + phdr[i].p_offset);

            vfs_node_t interpreter_node = vfs_open(interpreter_name);
            if (!interpreter_node)
            {
                can_schedule = true;
                execve_lock = false;
                return (uint64_t)-ENOENT;
            }

            uint8_t *interpreter_buffer = (uint8_t *)INTERPRETER_EHDR_ADDR;
            map_page_range(get_current_page_dir(true), INTERPRETER_EHDR_ADDR, 0, (interpreter_node->size + DEFAULT_PAGE_SIZE - 1) & (~(DEFAULT_PAGE_SIZE - 1)), PT_FLAG_R | PT_FLAG_W | PT_FLAG_U);

            vfs_read(interpreter_node, interpreter_buffer, 0, interpreter_node->size);

            vfs_close(interpreter_node);

            Elf64_Ehdr *interpreter_ehdr = (Elf64_Ehdr *)interpreter_buffer;
            Elf64_Phdr *interpreter_phdr = (Elf64_Phdr *)(interpreter_buffer + interpreter_ehdr->e_phoff);

            for (int j = 0; j < interpreter_ehdr->e_phnum; j++)
            {
                if (interpreter_phdr[j].p_type != PT_LOAD)
                    continue;

                uint64_t seg_addr = INTERPRETER_BASE_ADDR + interpreter_phdr[j].p_vaddr;
                uint64_t seg_size = interpreter_phdr[j].p_memsz;
                uint64_t file_size = interpreter_phdr[j].p_filesz;
                uint64_t page_size = DEFAULT_PAGE_SIZE;
                uint64_t page_mask = page_size - 1;

                // 计算对齐后的地址和大小
                uint64_t aligned_addr = seg_addr & ~page_mask;
                uint64_t size_diff = seg_addr - aligned_addr;
                uint64_t alloc_size = (seg_size + size_diff + page_mask) & ~page_mask;

                uint64_t flags = PT_FLAG_R | PT_FLAG_U | PT_FLAG_W | PT_FLAG_X;
                map_page_range(get_current_page_dir(true), aligned_addr, 0, alloc_size, flags);
                fast_memcpy((void *)seg_addr, (void *)(INTERPRETER_EHDR_ADDR + interpreter_phdr[j].p_offset), file_size);

                if (seg_size > file_size)
                {
                    uint64_t bss_start = seg_addr + file_size;
                    uint64_t bss_size = seg_size - file_size;
                    memset((void *)bss_start, 0, bss_size);

                    uint64_t page_remain = (bss_size % DEFAULT_PAGE_SIZE);
                    if (page_remain)
                    {
                        uint64_t align_start = bss_start + bss_size - page_remain;
                        memset((void *)align_start, 0, page_remain);
                    }
                }
            }

            interpreter_entry = INTERPRETER_BASE_ADDR + interpreter_ehdr->e_entry;
        }
        else
        {
            if (phdr[i].p_type != PT_LOAD)
                continue;

            uint64_t seg_addr = phdr[i].p_vaddr;
            uint64_t seg_size = phdr[i].p_memsz;
            uint64_t file_size = phdr[i].p_filesz;
            uint64_t page_size = DEFAULT_PAGE_SIZE;
            uint64_t page_mask = page_size - 1;

            // 计算对齐后的地址和大小
            uint64_t aligned_addr = seg_addr & ~page_mask;
            uint64_t size_diff = seg_addr - aligned_addr;
            uint64_t alloc_size = (seg_size + size_diff + page_mask) & ~page_mask;

            if (aligned_addr < load_start)
                load_start = aligned_addr;
            else if (aligned_addr + alloc_size > load_end)
                load_end = aligned_addr + alloc_size;

            uint64_t flags = PT_FLAG_R | PT_FLAG_U | PT_FLAG_W | PT_FLAG_X;
            map_page_range(get_current_page_dir(true), aligned_addr, 0, alloc_size, flags);
            fast_memcpy((void *)seg_addr, (void *)(EHDR_START_ADDR + phdr[i].p_offset), file_size);

            if (seg_size > file_size)
            {
                uint64_t bss_start = seg_addr + file_size;
                uint64_t bss_size = seg_size - file_size;
                memset((void *)bss_start, 0, bss_size);

                uint64_t page_remain = (bss_size % DEFAULT_PAGE_SIZE);
                if (page_remain)
                {
                    uint64_t align_start = bss_start + bss_size - page_remain;
                    memset((void *)align_start, 0, page_remain);
                }
            }
        }
    }

    strncpy(current_task->name, fullpath, TASK_NAME_MAX);
    free(fullpath);

    map_page_range(get_current_page_dir(true), USER_STACK_START, 0, USER_STACK_END - USER_STACK_START, PT_FLAG_R | PT_FLAG_W | PT_FLAG_U);

    uint64_t stack = push_infos(current_task, USER_STACK_END, (char **)new_argv, (char **)new_envp, e_entry, (uint64_t)(load_start + ehdr->e_phoff), ehdr->e_phnum, interpreter_entry ? INTERPRETER_BASE_ADDR : load_start);

    char cmdline[DEFAULT_PAGE_SIZE];
    memset(cmdline, 0, sizeof(cmdline));
    char *cmdline_ptr = cmdline;
    for (int i = 0; i < argv_count; i++)
    {
        int len = sprintf(cmdline_ptr, "%s ", new_argv[i]);
        cmdline_ptr += len;
    }

    for (int i = 0; i < argv_count; i++)
    {
        if (new_argv[i])
        {
            free(new_argv[i]);
        }
    }
    free(new_argv);
    for (int i = 0; i < envp_count; i++)
    {
        if (new_envp[i])
        {
            free(new_envp[i]);
        }
    }
    free(new_envp);

    for (uint64_t i = 3; i < MAX_FD_NUM; i++)
    {
        if (!current_task->fds[i])
            continue;

        if (current_task->fds[i]->flags & O_CLOEXEC)
        {
            vfs_close(current_task->fds[i]->node);
            free(current_task->fds[i]);
            current_task->fds[i] = NULL;
        }
    }

    current_task->cmdline = strdup(cmdline);
    current_task->load_start = load_start;
    current_task->load_end = load_end;

    execve_lock = false;
    can_schedule = true;

    arch_to_user_mode(current_task->arch_context, interpreter_entry ? interpreter_entry : e_entry, stack);

    return (uint64_t)-EAGAIN;
}

void sys_yield()
{
    arch_yield();
}

int task_block(task_t *task, task_state_t state, int timeout_ms)
{
    (void)timeout_ms;

    task->state = state;

    if (current_task == task)
    {
        arch_enable_interrupt();

        arch_pause();
    }

    arch_disable_interrupt();

    return task->status;
}

void task_unblock(task_t *task, int reason)
{
    task->status = reason;
    task->state = TASK_READY;
}

uint64_t task_exit(int64_t code)
{
    arch_disable_interrupt();

    task_t *task = current_task;

    arch_context_free(task->arch_context);

    free_frames_bytes((void *)task->kernel_stack, STACK_SIZE);
    free_frames_bytes((void *)task->syscall_stack, STACK_SIZE);

    task->status = (uint64_t)code;

    for (uint64_t i = 0; i < MAX_FD_NUM; i++)
    {
        if (task->fds[i])
        {
            vfs_close(task->fds[i]->node);
            free(task->fds[i]);

            task->fds[i] = NULL;
        }
    }

    if (task->waitpid != 0 && tasks[task->waitpid])
    {
        task_unblock(tasks[task->waitpid], EOK);
    }

    if (task->cmdline)
        free(task->cmdline);

    socket_on_exit_task(task->pid);

    task->state = TASK_DIED;

    task_t *next = task_search(TASK_READY, task->cpu_id);

    if (next)
    {
        arch_set_current(next);
        arch_switch_with_context(NULL, next->arch_context, next->kernel_stack);
    }
    else
    {
        arch_set_current(idle_tasks[current_cpu_id]);
        arch_switch_with_context(NULL, idle_tasks[current_cpu_id]->arch_context, idle_tasks[current_cpu_id]->kernel_stack);
    }

    // never return !!!

    return (uint64_t)-EAGAIN;
}

uint64_t sys_waitpid(uint64_t pid, int *status, uint64_t options)
{
    task_t *child = NULL;
    uint64_t ret = -ECHILD;

    while (1)
    {
        bool has_child = false;

        // 遍历所有任务查找符合条件的子进程
        for (uint64_t i = 1; i < MAX_TASK_NUM; i++)
        {
            task_t *ptr = tasks[i];
            if (!ptr || ptr->pid == ptr->ppid || ptr->ppid != current_task->pid)
                continue;

            // 处理不同pid参数情况
            if (pid == (uint64_t)-1)
            { // 任意子进程
                child = ptr;
                has_child = true;
                if (child->state == TASK_DIED)
                {
                    goto rollback;
                }
                break;
            }
            else if (pid == 0)
            { // 同进程组
                if (ptr->pgid == current_task->pgid)
                {
                    child = ptr;
                    has_child = true;
                    break;
                }
            }
            else if (pid > 0)
            { // 指定PID
                if (ptr->pid != pid)
                    continue;
            }

            // 检查进程状态
            if (ptr->state == TASK_DIED)
            {
                child = ptr;
                // tasks[i] = NULL;
                goto rollback;
            }
            else
            {
                child = ptr;
                has_child = true;
                break;
            }
        }

        if (!has_child || !child)
        {
            if (options & WNOHANG)
            {
                return 0;
            }
            break;
        }

        child->waitpid = current_task->pid;

        current_task->state = TASK_BLOCKING;

        while (current_task->state == TASK_BLOCKING)
        {
            arch_enable_interrupt();
            arch_pause();
        }
    }

rollback:
    if (child)
    {
        if (status)
        {
            if (child->status < 128)
            {
                *status = (child->status & 0xff) << 8;
            }
            else
            {
                int sig = child->status - 128;
                *status = sig | (0x80 << 8);
            }
        }

        ret = child->pid;

        tasks[child->pid] = NULL;

        free_page_table(child->arch_context->mm);

        free(child->arch_context);

        free(child);
    }
    else if (options & WNOHANG)
    {
        ret = 0;
    }

    return ret;
}

uint64_t sys_clone(struct pt_regs *regs, uint64_t flags, uint64_t newsp, int *parent_tid, int *child_tid, uint64_t tls)
{
    arch_disable_interrupt();

    can_schedule = false;

    task_t *child = get_free_task();
    if (child == NULL)
    {
        return (uint64_t)-ENOMEM;
    }

    strncpy(child->name, current_task->name, TASK_NAME_MAX);

    child->state = TASK_READY;
    child->current_state = TASK_READY;

    child->cpu_id = alloc_cpu_id();

    child->kernel_stack = (uint64_t)alloc_frames_bytes(STACK_SIZE) + STACK_SIZE;
    child->syscall_stack = (uint64_t)alloc_frames_bytes(STACK_SIZE) + STACK_SIZE;
    memset((void *)(child->kernel_stack - STACK_SIZE), 0, STACK_SIZE);
    memset((void *)(child->syscall_stack - STACK_SIZE), 0, STACK_SIZE);

    child->arch_context = malloc(sizeof(arch_context_t));
    memset(child->arch_context, 0, sizeof(arch_context_t));
    current_task->arch_context->ctx = regs;
    arch_context_copy(child->arch_context, current_task->arch_context, child->kernel_stack, flags);
#if defined(__x86_64__)
    if (newsp)
        child->arch_context->ctx->rsp = newsp;
#endif
    child->ppid = current_task->pid;
    child->uid = current_task->uid;
    child->gid = current_task->gid;
    child->euid = current_task->euid;
    child->egid = current_task->egid;
    child->pgid = current_task->pgid;

    child->jiffies = current_task->jiffies;

    child->cwd = current_task->cwd;
    child->cmdline = current_task->cmdline;

    child->mmap_start = USER_MMAP_START;
    child->brk_start = USER_BRK_START;
    child->brk_end = USER_BRK_START;
    child->load_start = current_task->load_start;
    child->load_end = current_task->load_end;

    memset(child->fds, 0, sizeof(child->fds));
    child->fds[0] = malloc(sizeof(fd_t));
    child->fds[0]->node = vfs_open("/dev/stdin");
    child->fds[0]->offset = 0;
    child->fds[0]->flags = 0;
    child->fds[1] = malloc(sizeof(fd_t));
    child->fds[1]->node = vfs_open("/dev/stdout");
    child->fds[1]->offset = 0;
    child->fds[1]->flags = 0;
    child->fds[2] = malloc(sizeof(fd_t));
    child->fds[2]->node = vfs_open("/dev/stderr");
    child->fds[2]->offset = 0;
    child->fds[2]->flags = 0;

    for (uint64_t i = 3; i < MAX_FD_NUM; i++)
    {
        if (current_task->fds[i])
        {
            child->fds[i] = vfs_dup(current_task->fds[i]);
        }
        else
        {
            child->fds[i] = NULL;
        }
    }

    memcpy(&child->term, &current_task->term, sizeof(termios));

    if (flags & CLONE_SIGHAND)
    {
        memcpy(child->actions, current_task->actions, sizeof(child->actions));
        child->signal = current_task->signal;
        child->blocked = current_task->blocked;
    }
    else
    {
        memset(child->actions, 0, sizeof(child->actions));
    }

    if (flags & CLONE_SETTLS)
    {
#if defined(__x86_64__)
        child->arch_context->fsbase = tls;
#endif
    }

    if (flags & CLONE_PARENT_SETTID)
    {
        *parent_tid = (int)current_task->pid;
    }

    if (flags & CLONE_CHILD_SETTID)
    {
        *child_tid = (int)child->pid;
    }

    child->tmp_rec_v = current_task->tmp_rec_v;

    memcpy(child->rlim, current_task->rlim, sizeof(child->rlim));

    socket_on_new_task(child->pid);

    can_schedule = true;

    arch_enable_interrupt();

    return child->pid;
}

uint64_t sys_nanosleep(struct timespec *req, struct timespec *rem)
{
    if (req->tv_sec < 0)
        return (uint64_t)-EINVAL;

    if (req->tv_sec < 0 || req->tv_nsec >= 1000000000L)
    {
        return (uint64_t)-EINVAL;
    }

    uint64_t start = nanoTime();
    uint64_t target = start + (req->tv_sec * 1000000000ULL) + req->tv_nsec;

    do
    {
        if (signals_pending_quick(current_task))
        {
            if (rem)
            {
                uint64_t remaining = target - nanoTime();
                struct timespec remain_ts = {
                    .tv_sec = remaining / 1000000000,
                    .tv_nsec = remaining % 1000000000};
                memcpy(rem, &remain_ts, sizeof(struct timespec));
            }
            return (uint64_t)-EINTR;
        }

        arch_enable_interrupt();

        arch_pause();
    } while (target > nanoTime());

    arch_disable_interrupt();

    return 0;
}

uint64_t sys_prctl(uint64_t option, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    switch (option)
    {
    case PR_SET_NAME: // 设置进程名 (PR_SET_NAME=15)
        strncpy(current_task->name, (char *)arg2, TASK_NAME_MAX);
        return 0;

    case PR_GET_NAME: // 获取进程名 (PR_GET_NAME=16)
        strncpy((char *)arg2, current_task->name, TASK_NAME_MAX);
        return 0;

    case PR_SET_SECCOMP: // 启用seccomp过滤
        if (arg2 == SECCOMP_MODE_STRICT)
        {
            // current_task->seccomp_mode = SECCOMP_MODE_STRICT;
            return 0;
        }
        return -EINVAL;

    case PR_GET_SECCOMP: // 查询seccomp状态
        // return current_task->seccomp_mode;
        return 0;

    case PR_SET_TIMERSLACK:
        current_task->timer_slack_ns = arg2;
        return 0;

    default:
        return -ENOSYS; // 未实现的功能返回不支持
    }
}

void ms_to_timeval(uint64_t ms, struct timeval *tv)
{
    tv->tv_sec = ms / 1000;
    tv->tv_usec = (ms % 1000) * 1000;
}

uint64_t timeval_to_ms(struct timeval tv)
{
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

extern int timerfdfs_id;

void sched_update_itimer()
{
    for (uint64_t i = 1; i < MAX_TASK_NUM; i++)
    {
        task_t *ptr = tasks[i];

        if (!ptr)
            break;

        uint64_t rtAt = ptr->itimer_real.at;
        uint64_t rtReset = ptr->itimer_real.reset;

        if (rtAt && rtAt <= jiffies)
        {
            ptr->signal |= SIGMASK(SIGALRM);
            if (ptr->state == TASK_BLOCKING)
                task_unblock(ptr, EOK);

            if (rtReset)
            {
                ptr->itimer_real.at = jiffies + rtReset;
            }
            else
            {
                ptr->itimer_real.at = 0;
            }
        }

        for (int j = 0; j < MAX_TIMERS_NUM; j++)
        {
            if (ptr->timers[i] == NULL)
                continue;
            kernel_timer_t *kt = ptr->timers[j];
            if (kt->expires && jiffies >= kt->expires)
            {
                ptr->signal |= SIGMASK(kt->sigev_signo);

                if (kt->interval)
                    kt->expires += kt->interval;
                else
                    kt->expires = 0;
            }
            if (current_task->fds[i] && current_task->fds[i]->node && current_task->fds[i]->node->fsid == timerfdfs_id)
            {
                timerfd_t *tfd = current_task->fds[i]->node->handle;
                if (tfd->timer.expires && jiffies >= tfd->timer.expires)
                {
                    tfd->count++;
                    if (tfd->timer.interval)
                    {
                        tfd->timer.expires += tfd->timer.interval;
                    }
                    else
                    {
                        tfd->timer.expires = 0;
                    }
                }
            }
        }
    }
}

size_t sys_setitimer(int which, struct itimerval *value, struct itimerval *old)
{
    if (which != 0)
    {
        return (size_t)-ENOSYS;
    }

    uint64_t rt_at = current_task->itimer_real.at;
    uint64_t rt_reset = current_task->itimer_real.reset;

    if (old)
    {
        uint64_t realValue = rt_at - jiffies;
        ms_to_timeval(realValue, &old->it_value);
        ms_to_timeval(rt_reset, &old->it_interval);
    }

    if (value)
    {
        uint64_t targValue = timeval_to_ms(value->it_value);
        uint64_t targInterval = timeval_to_ms(value->it_interval);

        if (targValue)
            current_task->itimer_real.at = jiffies + targValue;
        else
            current_task->itimer_real.at = 0ULL;

        current_task->itimer_real.reset = targInterval;
    }

    return 0;
}

int sys_timer_create(clockid_t clockid, struct sigevent *sevp, timer_t *timerid)
{
    kernel_timer_t *kt = NULL;
    uint64_t i;
    for (i = 0; i < MAX_TIMERS_NUM; i++)
    {
        if (current_task->timers[i] == NULL)
        {
            kt = malloc(sizeof(kernel_timer_t));
            current_task->timers[i] = kt;
            break;
        }
    }

    if (!kt)
        return -ENOMEM;

    memset(kt, 0, sizeof(kernel_timer_t));

    kt->clock_type = clockid;
    kt->sigev_notify = SIGEV_SIGNAL;

    if (sevp)
    {
        struct sigevent ksev;
        memcpy(&ksev, sevp, sizeof(struct sigevent));

        kt->sigev_signo = ksev.sigev_signo;
        kt->sigev_value = ksev.sigev_value;
        kt->sigev_notify = ksev.sigev_notify;
    }

    *timerid = (timer_t)i;

    return 0;
}

int sys_timer_settime(timer_t timerid, const struct itimerval *new_value, struct itimerval *old_value)
{
    uint64_t idx = (uint64_t)timerid;
    if (idx >= MAX_TIMERS_NUM)
        return -EINVAL;

    kernel_timer_t *kt = current_task->timers[idx];

    struct itimerval kts;
    memcpy(&kts, new_value, sizeof(*new_value));

    uint64_t now = jiffies;
    uint64_t interval = kts.it_interval.tv_sec * 1000 + kts.it_interval.tv_usec / 1000000;
    uint64_t expires = kts.it_value.tv_sec * 1000 + kts.it_value.tv_usec / 1000000;

    if (old_value)
    {
        struct itimerval old;
        old.it_interval.tv_sec = kt->interval / 1000;
        old.it_interval.tv_usec = (kt->interval % 1000) * 1000000;
        old.it_value.tv_sec = (kt->expires - now) / 1000;
        old.it_value.tv_usec = ((kt->expires - now) % 1000) * 1000000;
        memcpy(old_value, &old, sizeof(old));
    }

    kt->interval = interval;
    kt->expires = now + expires;

    return 0;
}
