/* Link validation only: no SELF/VPK is generated and no device deployment uses
 * this target. Forced linker roots retain the complete execution dependency graph. */
int module_start(unsigned args, const void *argp)
{
    (void)args;
    (void)argp;
    return 2;
}
int module_stop(unsigned args, const void *argp)
{
    (void)args;
    (void)argp;
    return 0;
}
