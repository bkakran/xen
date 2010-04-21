#include <xenctrl.h>
#include <xc_private.h>
#include <xc_core.h>

#define ARRAY_SIZE(a) (sizeof (a) / sizeof ((a)[0]))

static int xc_fd;

/* help message */
void show_help(void)
{
    fprintf(stderr,
            "\nxen cpu memory hotplug tool\n\n"
            "usage: xen-hptool <command> [args]\n\n"
            "xen-hptool command list:\n\n"
            "cpu-online    <cpuid>    online CPU <cpuid>\n"
            "cpu-offline   <cpuid>    offline CPU <cpuid>\n"
            "mem-online    <mfn>      online MEMORY <mfn>\n"
            "mem-offline   <mfn>      offline MEMORY <mfn>\n"
            "mem-status    <mfn>      query Memory status<mfn>\n"
           );
}

/* wrapper function */
static int help_func(int argc, char *argv[])
{
    show_help();
    return 0;
}

static int hp_mem_online_func(int argc, char *argv[])
{
    uint32_t status;
    int ret;
    unsigned long mfn;

    if (argc != 1)
    {
        show_help();
        return -1;
    }

    sscanf(argv[0], "%lx", &mfn);
    printf("Prepare to online MEMORY mfn %lx\n", mfn);

    ret = xc_mark_page_online(xc_fd, mfn, mfn, &status);

    if (ret < 0)
        fprintf(stderr, "Onlining page mfn %lx failed, error %x", mfn, ret);
    else if (status & (PG_ONLINE_FAILED |PG_ONLINE_BROKEN)) {
        fprintf(stderr, "Onlining page mfn %lx is broken, "
                        "Memory online failed\n", mfn);
        ret = -1;
	}
    else if (status & PG_ONLINE_ONLINED)
        printf("Memory mfn %lx onlined successfully\n", mfn);
    else
        printf("Memory is already onlined!\n");

    return ret;
}

static int hp_mem_query_func(int argc, char *argv[])
{
    uint32_t status;
    int ret;
    unsigned long mfn;

    if (argc != 1)
    {
        show_help();
        return -1;
    }

    sscanf(argv[0], "%lx", &mfn);
    printf("Querying MEMORY mfn %lx status\n", mfn);
    ret = xc_query_page_offline_status(xc_fd, mfn, mfn, &status);

    if (ret < 0)
        fprintf(stderr, "Querying page mfn %lx failed, error %x", mfn, ret);
    else
    {
		printf("Memory Status %x: [", status);
        if ( status & PG_OFFLINE_STATUS_OFFLINE_PENDING)
            printf(" PAGE_OFFLINE_PENDING ");
        if ( status & PG_OFFLINE_STATUS_BROKEN )
            printf(" PAGE_BROKEND  ");
        if ( status & PG_OFFLINE_STATUS_OFFLINED )
            printf(" PAGE_OFFLINED ");
		else
            printf(" PAGE_ONLINED ");
        printf("]\n");
    }

    return ret;
}

extern int xs_suspend_evtchn_port(int domid);

static int suspend_guest(int xc_handle, int xce, int domid, int *evtchn)
{
    int port, rc, suspend_evtchn = -1;

    if (!evtchn)
        return -1;

    port = xs_suspend_evtchn_port(domid);
    if (port < 0)
    {
        fprintf(stderr, "DOM%d: No suspend port, try live migration\n", domid);
        goto failed;
    }
    suspend_evtchn = xc_suspend_evtchn_init(xc_handle, xce, domid, port);
    if (suspend_evtchn < 0)
    {
        fprintf(stderr, "Suspend evtchn initialization failed\n");
        goto failed;
    }
    *evtchn = suspend_evtchn;

    rc = xc_evtchn_notify(xce, suspend_evtchn);
    if (rc < 0)
    {
        fprintf(stderr, "Failed to notify suspend channel: errno %d\n", rc);
        goto failed;
    }
    if (xc_await_suspend(xce, suspend_evtchn) < 0)
    {
        fprintf(stderr, "Suspend Failed\n");
        goto failed;
    }
    return 0;

failed:
    if (suspend_evtchn != -1)
        xc_suspend_evtchn_release(xce, suspend_evtchn);

    return -1;
}

static int hp_mem_offline_func(int argc, char *argv[])
{
    uint32_t status, domid;
    int ret;
    unsigned long mfn;

    if (argc != 1)
    {
        show_help();
        return -1;
    }

    sscanf(argv[0], "%lx", &mfn);
    printf("Prepare to offline MEMORY mfn %lx\n", mfn);
    ret = xc_mark_page_offline(xc_fd, mfn, mfn, &status);
    if (ret < 0) {
        fprintf(stderr, "Offlining page mfn %lx failed, error %x\n", mfn, ret);
        if (status & (PG_OFFLINE_XENPAGE | PG_OFFLINE_FAILED))
            fprintf(stderr, "XEN_PAGE is not permitted be offlined\n");
        else if (status & (PG_OFFLINE_FAILED | PG_OFFLINE_NOT_CONV_RAM))
            fprintf(stderr, "RESERVED RAM is not permitted to be offlined\n");
    }
    else
    {
        switch(status & PG_OFFLINE_STATUS_MASK)
        {
            case PG_OFFLINE_OFFLINED:
            {
                printf("Memory mfn %lx offlined successfully, current state is"
                       " [PG_OFFLINE_OFFLINED]\n", mfn);
                if (status & PG_OFFLINE_BROKEN)
                    printf("And this offlined PAGE is already marked broken"
                        " before!\n");
                break;
            }
            case PG_OFFLINE_FAILED:
            {
                fprintf(stderr, "Memory mfn %lx offline failed\n", mfn);
                if ( status & PG_OFFLINE_ANONYMOUS)
                    fprintf(stderr, "the memory is an anonymous page!\n");
                ret = -1;
                break;
            }
            case PG_OFFLINE_PENDING:
            {
                if (status & PG_OFFLINE_XENPAGE) {
                    ret = -1;
                    fprintf(stderr, "Memory mfn %lx offlined succssefully,"
                            "this page is xen page, current state is"
                            " [PG_OFFLINE_PENDING, PG_OFFLINE_XENPAGE]\n", mfn);
                }
                else if (status & PG_OFFLINE_OWNED)
                {
                    int result, xce, suspend_evtchn = -1;
                    xce = xc_evtchn_open();

                    if (xce < 0)
                    {
                        fprintf(stderr, "When exchange page, fail"
                                " to open evtchn\n");
                        return -1;
                    }

                    domid = status >> PG_OFFLINE_OWNER_SHIFT;
                    if (suspend_guest(xc_fd, xce, domid, &suspend_evtchn))
                    {
                        fprintf(stderr, "Failed to suspend guest %d for"
                                " mfn %lx\n", domid, mfn);
                        xc_evtchn_close(xce);
                        return -1;
                    }

                    result = xc_exchange_page(xc_fd, domid, mfn);

                    /* Exchange page successfully */
                    if (result == 0)
                        printf("Memory mfn %lx offlined successfully, this "
                                "page is DOM%d page and being swapped "
                                "successfully, current state is "
                                "[PG_OFFLINE_OFFLINED, PG_OFFLINE_OWNED]\n",
                                mfn, domid);
                    else {
                        ret = -1;
                        fprintf(stderr, "Memory mfn %lx offlined successfully"
                                " , this page is DOM%d page yet failed to be "
                                "exchanged. current state is "
                                "[PG_OFFLINE_PENDING, PG_OFFLINE_OWNED]\n",
                                mfn, domid);
                    }
                    xc_domain_resume(xc_fd, domid, 1);
                    xc_suspend_evtchn_release(xce, suspend_evtchn);
                    xc_evtchn_close(xce);
                }
                break;
            }
        }//end of switch
    }//end of if

    return ret;
}


static int hp_cpu_online_func(int argc, char *argv[])
{
    int cpu, ret;

    if (argc != 1)
    {
        show_help();
        return -1;
    }

    cpu = atoi(argv[0]);
    printf("Prepare to online CPU %d\n", cpu);
    ret = xc_cpu_online(xc_fd, cpu);
    if (ret < 0)
        fprintf(stderr, "CPU %d onlined failed\n", cpu);
    else
        printf("CPU %d onlined successfully\n", cpu);

    return ret;

}
static int hp_cpu_offline_func(int argc, char *argv[])
{
    int cpu, ret;

    if (argc !=1)
    {
        show_help();
        return -1;
    }
    cpu = atoi(argv[0]);
    printf("Prepare to offline CPU %d\n", cpu);
    ret = xc_cpu_offline(xc_fd, cpu);
    if (ret < 0)
        fprintf(stderr, "CPU %d offlined failed\n", cpu);
    else
        printf("CPU %d offlined successfully\n", cpu);

    return ret;
}

struct {
    const char *name;
    int (*function)(int argc, char *argv[]);
} main_options[] = {
    { "help", help_func },
    { "cpu-online", hp_cpu_online_func },
    { "cpu-offline", hp_cpu_offline_func },
    { "mem-status", hp_mem_query_func},
    { "mem-online", hp_mem_online_func},
    { "mem-offline", hp_mem_offline_func},
};


int main(int argc, char *argv[])
{
    int i, ret;
    if (argc < 2)
    {
        show_help();
        return 0;
    }

    xc_fd = xc_interface_open();
    if ( xc_fd < 0 )
    {
        fprintf(stderr, "failed to get the handler\n");
        return 0;
    }

    for (i = 0; i < ARRAY_SIZE(main_options); i++)
    {
        if (!strncmp(main_options[i].name, argv[1], strlen(argv[1])))
        {
            ret = main_options[i].function(argc -2, argv + 2);
            break;
        }
    }
    if (i >= ARRAY_SIZE(main_options))
        show_help();
    else if (ret < 0)
        fprintf(stderr, "Required Ops failed\n");

    xc_interface_close(xc_fd);

    return 0;
}