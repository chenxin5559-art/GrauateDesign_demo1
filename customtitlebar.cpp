// customtitlebar.cpp
#include "customtitlebar.h"
#include <QHBoxLayout>
#include <QIcon>
#include <QPixmap>  // 新增 QPixmap 头文件（用于加载图标）

CustomTitleBar::CustomTitleBar(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedHeight(40);
    setStyleSheet("background: #f0f0f0; border-bottom: 1px solid #ddd;");

    // customtitlebar.cpp（修改图标初始化部分）
    m_iconLabel = new QLabel(this);
    const int iconDisplaySize = 30; // 图标显示尺寸，可根据标题栏高度调整（当前标题栏高度40，32比较合适）
    m_iconLabel->setFixedSize(iconDisplaySize, iconDisplaySize);

    // 加载图标并平滑缩放（保持宽高比，避免模糊变形）
    QPixmap originalIcon(":/images/images/software-icon.png");
    QPixmap scaledIcon = originalIcon.scaled(
        iconDisplaySize,
        iconDisplaySize,
        Qt::KeepAspectRatio, // 保持宽高比，避免图标变形
        Qt::SmoothTransformation // 平滑缩放，提升清晰度
        );
    m_iconLabel->setPixmap(scaledIcon);
    m_iconLabel->setAlignment(Qt::AlignCenter); // 图标在标签内居中显示


    // 标题标签
    m_titleLabel = new QLabel("红外测温仪自动标校软件", this);
    m_titleLabel->setStyleSheet("font-size: 18px; color: #333; font-weight: bold;");
    m_titleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_titleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // 创建按钮（保持原有逻辑）
    m_minimizeButton = createCustomButton(":/images/images/Minimize.png", "最小化");
    m_maximizeRestoreButton = createCustomButton(":/images/images/Maximize.png", "最大化/还原");
    m_closeButton = createCustomButton(":/images/images/close.png", "关闭");
    m_closeButton->setStyleSheet(
        "QPushButton {"
        "    border: none;"
        "    background: transparent;"
        "    padding: 0;"
        "}"
        "QPushButton:hover {"
        "    background: #CE0000;" // 红色背景
        "}"
        "QPushButton:pressed {"
        "    background: #d0d0d0;"
        "}"
        );

    // 连接信号（保持原有逻辑）
    connect(m_minimizeButton, &QPushButton::clicked, this, &CustomTitleBar::minimizeClicked);
    connect(m_maximizeRestoreButton, &QPushButton::clicked, this, &CustomTitleBar::maximizeRestoreClicked);
    connect(m_closeButton, &QPushButton::clicked, this, &CustomTitleBar::closeClicked);

    initLayout();
}

// 创建通用按钮（保持原有逻辑）
QPushButton* CustomTitleBar::createCustomButton(const QString &iconPath, const QString &toolTip) {
    QPushButton *button = new QPushButton(this);
    button->setFixedSize(25, 25);
    button->setIcon(QIcon(iconPath));
    button->setIconSize(QSize(16, 16));
    button->setToolTip(toolTip);

    // 显式设置按钮所有状态无边框（覆盖默认样式）
    button->setStyleSheet(
        "QPushButton {"
        "    border: none;          /* 移除按钮边框 */"
        "    background: transparent; /* 背景透明 */"
        "    padding: 0;            /* 移除内边距 */"
        "}"
        "QPushButton:hover {"       /* 悬停时样式 */
        "    background: #e0e0e0;   /* 可选：悬停时背景色（可选） */"
        "}"
        "QPushButton:pressed {"     /* 按下时样式 */
        "    background: #d0d0d0;   /* 可选：按下时背景色（可选） */"
        "}"
        );
    return button;
}

// 初始化布局（调整布局顺序，新增图标）
void CustomTitleBar::initLayout() {
    QHBoxLayout *mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(8, 0, 8, 0);
    mainLayout->setSpacing(12);

    // 布局顺序：图标 → 标题 → 弹簧 → 按钮
    mainLayout->addWidget(m_iconLabel);    // 新增：软件图标
    mainLayout->addWidget(m_titleLabel);   // 标题
    mainLayout->addStretch();              // 弹簧（使按钮靠右）

    // 右侧按钮布局（保持原有逻辑）
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->setSpacing(0);
    buttonLayout->addWidget(m_minimizeButton);
    buttonLayout->addWidget(m_maximizeRestoreButton);
    buttonLayout->addWidget(m_closeButton);

    mainLayout->addLayout(buttonLayout);
    setLayout(mainLayout);
}

// 鼠标拖动窗口（保持原有逻辑）
void CustomTitleBar::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        lastPos = event->globalPos() - window()->pos();
        event->accept();
    }
}

void CustomTitleBar::mouseMoveEvent(QMouseEvent *event) {
    if (event->buttons() & Qt::LeftButton) {
        window()->move(event->globalPos() - lastPos);
        event->accept();
    }
}
