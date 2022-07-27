#include <stdio.h>
#include <unistd.h>
#include <string>
#include"config.h"

Config::Config(){
    PORT = 10000;
    ActorMode = 0;
    TrigMode = 0;
}

void Config::parse_arg(int argc, char* argv[]){
    int opt;
    const char *str = "p:m:a:";
    while ((opt = getopt(argc, argv, str)) != -1)
    {
        switch (opt)
        {
        case 'p':
        {
            PORT = atoi(optarg);
            break;
        }
        case 'm':
        {
            TrigMode = atoi(optarg);
            break;
        }
        case 'a':
        {
            ActorMode = atoi(optarg);
            break;
        }
        default:
            break;
        }
    }
}