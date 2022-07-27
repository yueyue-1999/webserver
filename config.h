#ifndef CONFIG_H
#define CONFIG_H

// 用于解析shell的命令行参数
class Config{
public:
    Config();
    ~Config(){};

    void parse_arg(int argc, char* argv[]);

    int PORT;

    // Reactor模式还是Proactor模式
    int ActorMode;

    // 组合触发模式
    int TrigMode;
};

#endif 