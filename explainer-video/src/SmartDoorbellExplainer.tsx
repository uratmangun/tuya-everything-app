import React from "react";
import {
    useCurrentFrame,
    useVideoConfig,
    spring,
    interpolate,
    Sequence,
    AbsoluteFill
} from "remotion";

// ============== STYLE CONSTANTS ==============
const COLORS = {
    primary: "#0ea5e9",      // Cyan
    secondary: "#10b981",    // Green  
    accent: "#f59e0b",       // Orange
    dark: "#0f172a",         // Dark blue
    light: "#f8fafc",        // Light
    gradient1: "#0ea5e9",
    gradient2: "#10b981",
};

// ============== SHARED COMPONENTS ==============
const AnimatedText: React.FC<{
    text: string;
    delay?: number;
    size?: number;
    color?: string;
}> = ({ text, delay = 0, size = 60, color = COLORS.light }) => {
    const frame = useCurrentFrame();
    const { fps } = useVideoConfig();

    const opacity = spring({
        fps,
        frame: frame - delay,
        config: { damping: 100 },
        durationInFrames: 20,
    });

    const translateY = interpolate(
        spring({ fps, frame: frame - delay, config: { damping: 100 } }),
        [0, 1],
        [50, 0]
    );

    return (
        <div style={{
            opacity: Math.max(0, opacity),
            transform: `translateY(${translateY}px)`,
            fontSize: size,
            fontWeight: 700,
            color,
            textAlign: "center",
            fontFamily: "Inter, system-ui, sans-serif",
        }}>
            {text}
        </div>
    );
};

const FeatureCard: React.FC<{
    icon: string;
    title: string;
    description: string;
    delay?: number;
}> = ({ icon, title, description, delay = 0 }) => {
    const frame = useCurrentFrame();
    const { fps } = useVideoConfig();

    const scale = spring({
        fps,
        frame: frame - delay,
        config: { damping: 80 },
    });

    return (
        <div style={{
            transform: `scale(${Math.max(0, scale)})`,
            background: "rgba(255,255,255,0.1)",
            backdropFilter: "blur(10px)",
            borderRadius: 20,
            padding: 30,
            width: 350,
            textAlign: "center",
            border: "1px solid rgba(255,255,255,0.2)",
        }}>
            <div style={{ fontSize: 60, marginBottom: 15 }}>{icon}</div>
            <div style={{
                fontSize: 24,
                fontWeight: 700,
                color: COLORS.light,
                marginBottom: 10,
                fontFamily: "Inter, sans-serif"
            }}>
                {title}
            </div>
            <div style={{
                fontSize: 16,
                color: "rgba(255,255,255,0.8)",
                fontFamily: "Inter, sans-serif",
                lineHeight: 1.5,
            }}>
                {description}
            </div>
        </div>
    );
};

// ============== SCENE COMPONENTS ==============

// Scene 1: Title (0-5 seconds, frames 0-150)
const TitleScene: React.FC = () => {
    const frame = useCurrentFrame();
    const { fps } = useVideoConfig();

    const logoScale = spring({
        fps,
        frame,
        config: { damping: 50 },
    });

    return (
        <AbsoluteFill style={{
            background: `linear-gradient(135deg, ${COLORS.dark} 0%, #1e3a5f 100%)`,
            justifyContent: "center",
            alignItems: "center",
            flexDirection: "column",
        }}>
            <div style={{
                transform: `scale(${logoScale})`,
                fontSize: 100,
                marginBottom: 30,
            }}>
                🔔
            </div>
            <AnimatedText text="AI Smart Doorbell" size={80} delay={15} />
            <AnimatedText
                text="Powered by Tuya T5AI"
                size={32}
                delay={30}
                color="rgba(255,255,255,0.7)"
            />
        </AbsoluteFill>
    );
};

// Scene 2: Problem (5-12 seconds, frames 150-360)
const ProblemScene: React.FC = () => {
    const frame = useCurrentFrame();

    return (
        <AbsoluteFill style={{
            background: `linear-gradient(135deg, #1e293b 0%, ${COLORS.dark} 100%)`,
            justifyContent: "center",
            alignItems: "center",
            flexDirection: "column",
            padding: 80,
        }}>
            <AnimatedText text="The Problem" size={50} color={COLORS.accent} delay={0} />
            <div style={{ height: 40 }} />

            <div style={{
                display: "flex",
                gap: 60,
                justifyContent: "center",
                flexWrap: "wrap",
            }}>
                <FeatureCard
                    icon="🔒"
                    title="Vendor Lock-in"
                    description="Ring & Nest require proprietary cameras"
                    delay={20}
                />
                <FeatureCard
                    icon="💸"
                    title="Monthly Fees"
                    description="$3-12/month subscription costs"
                    delay={35}
                />
                <FeatureCard
                    icon="🚫"
                    title="Limited AI"
                    description="Advanced features locked behind paywalls"
                    delay={50}
                />
            </div>
        </AbsoluteFill>
    );
};

// Scene 3: Solution Intro (12-18 seconds, frames 360-540)
const SolutionScene: React.FC = () => {
    const frame = useCurrentFrame();
    const { fps } = useVideoConfig();

    const pulseScale = 1 + Math.sin(frame * 0.1) * 0.05;

    return (
        <AbsoluteFill style={{
            background: `linear-gradient(135deg, ${COLORS.primary} 0%, ${COLORS.secondary} 100%)`,
            justifyContent: "center",
            alignItems: "center",
            flexDirection: "column",
        }}>
            <div style={{
                transform: `scale(${pulseScale})`,
                fontSize: 100,
                marginBottom: 30,
            }}>
                ✨
            </div>
            <AnimatedText text="Our Solution" size={70} delay={0} />
            <div style={{ height: 20 }} />
            <AnimatedText
                text="Open-Source • AI-Powered • No Subscriptions"
                size={32}
                delay={20}
            />
        </AbsoluteFill>
    );
};

// Scene 4: Features Part 1 (18-28 seconds, frames 540-840)
const FeaturesScene1: React.FC = () => {
    return (
        <AbsoluteFill style={{
            background: `linear-gradient(135deg, ${COLORS.dark} 0%, #1e3a5f 100%)`,
            justifyContent: "center",
            alignItems: "center",
            flexDirection: "column",
            padding: 60,
        }}>
            <AnimatedText text="Key Features" size={50} color={COLORS.primary} delay={0} />
            <div style={{ height: 50 }} />

            <div style={{
                display: "flex",
                gap: 40,
                justifyContent: "center",
                flexWrap: "wrap",
            }}>
                <FeatureCard
                    icon="📹"
                    title="Any Camera"
                    description="Works with any RTSP/IP camera you own"
                    delay={15}
                />
                <FeatureCard
                    icon="🧠"
                    title="AI Detection"
                    description="Person detection & face recognition"
                    delay={30}
                />
                <FeatureCard
                    icon="🎤"
                    title="Two-Way Audio"
                    description="Talk to visitors from anywhere"
                    delay={45}
                />
                <FeatureCard
                    icon="🤖"
                    title="AI Responses"
                    description="Auto-reply when you're away"
                    delay={60}
                />
            </div>
        </AbsoluteFill>
    );
};

// Scene 5: How It Works (28-40 seconds, frames 840-1200)
const HowItWorksScene: React.FC = () => {
    const frame = useCurrentFrame();
    const { fps } = useVideoConfig();

    const step1Opacity = spring({ fps, frame, config: { damping: 100 } });
    const step2Opacity = spring({ fps, frame: frame - 30, config: { damping: 100 } });
    const step3Opacity = spring({ fps, frame: frame - 60, config: { damping: 100 } });
    const step4Opacity = spring({ fps, frame: frame - 90, config: { damping: 100 } });

    const arrowProgress = interpolate(frame, [30, 60, 90, 120], [0, 1, 2, 3], {
        extrapolateLeft: "clamp",
        extrapolateRight: "clamp",
    });

    return (
        <AbsoluteFill style={{
            background: `linear-gradient(135deg, #1e293b 0%, ${COLORS.dark} 100%)`,
            justifyContent: "center",
            alignItems: "center",
            flexDirection: "column",
            padding: 60,
        }}>
            <AnimatedText text="How It Works" size={50} color={COLORS.secondary} delay={0} />
            <div style={{ height: 60 }} />

            <div style={{
                display: "flex",
                alignItems: "center",
                gap: 30,
            }}>
                {/* Step 1: Doorbell */}
                <div style={{ opacity: Math.max(0, step1Opacity), textAlign: "center" }}>
                    <div style={{ fontSize: 60, marginBottom: 10 }}>🔔</div>
                    <div style={{ color: COLORS.light, fontSize: 18, fontFamily: "Inter, sans-serif" }}>
                        Visitor Presses<br />Doorbell
                    </div>
                </div>

                <div style={{ fontSize: 40, color: arrowProgress >= 1 ? COLORS.primary : "#333" }}>→</div>

                {/* Step 2: DevKit */}
                <div style={{ opacity: Math.max(0, step2Opacity), textAlign: "center" }}>
                    <div style={{ fontSize: 60, marginBottom: 10 }}>📟</div>
                    <div style={{ color: COLORS.light, fontSize: 18, fontFamily: "Inter, sans-serif" }}>
                        T5AI DevKit<br />Alerts VPS
                    </div>
                </div>

                <div style={{ fontSize: 40, color: arrowProgress >= 2 ? COLORS.primary : "#333" }}>→</div>

                {/* Step 3: AI */}
                <div style={{ opacity: Math.max(0, step3Opacity), textAlign: "center" }}>
                    <div style={{ fontSize: 60, marginBottom: 10 }}>🧠</div>
                    <div style={{ color: COLORS.light, fontSize: 18, fontFamily: "Inter, sans-serif" }}>
                        AI Captures<br />& Identifies
                    </div>
                </div>

                <div style={{ fontSize: 40, color: arrowProgress >= 3 ? COLORS.primary : "#333" }}>→</div>

                {/* Step 4: You */}
                <div style={{ opacity: Math.max(0, step4Opacity), textAlign: "center" }}>
                    <div style={{ fontSize: 60, marginBottom: 10 }}>📱</div>
                    <div style={{ color: COLORS.light, fontSize: 18, fontFamily: "Inter, sans-serif" }}>
                        You Respond<br />or AI Does
                    </div>
                </div>
            </div>
        </AbsoluteFill>
    );
};

// Scene 6: Tech Stack (40-47 seconds, frames 1200-1410)
const TechStackScene: React.FC = () => {
    return (
        <AbsoluteFill style={{
            background: `linear-gradient(135deg, ${COLORS.dark} 0%, #1e3a5f 100%)`,
            justifyContent: "center",
            alignItems: "center",
            flexDirection: "column",
            padding: 60,
        }}>
            <AnimatedText text="Built With" size={50} color={COLORS.accent} delay={0} />
            <div style={{ height: 50 }} />

            <div style={{
                display: "flex",
                gap: 50,
                justifyContent: "center",
                flexWrap: "wrap",
            }}>
                <FeatureCard
                    icon="⚡"
                    title="Tuya T5AI"
                    description="8MB RAM, WiFi, Audio I/O"
                    delay={15}
                />
                <FeatureCard
                    icon="☁️"
                    title="VPS Backend"
                    description="Node.js + WebSocket"
                    delay={30}
                />
                <FeatureCard
                    icon="🔮"
                    title="AI Engine"
                    description="YOLO + GPT Integration"
                    delay={45}
                />
            </div>
        </AbsoluteFill>
    );
};

// Scene 7: Call to Action (47-50 seconds, frames 1410-1500)
const CTAScene: React.FC = () => {
    const frame = useCurrentFrame();
    const pulseScale = 1 + Math.sin(frame * 0.15) * 0.03;

    return (
        <AbsoluteFill style={{
            background: `linear-gradient(135deg, ${COLORS.primary} 0%, ${COLORS.secondary} 100%)`,
            justifyContent: "center",
            alignItems: "center",
            flexDirection: "column",
        }}>
            <div style={{
                transform: `scale(${pulseScale})`,
                fontSize: 80,
                marginBottom: 30,
            }}>
                🚀
            </div>
            <AnimatedText text="Open Source" size={70} delay={0} />
            <div style={{ height: 20 }} />
            <AnimatedText text="Build Your Own Smart Doorbell" size={36} delay={15} />
            <div style={{ height: 40 }} />
            <AnimatedText
                text="github.com/uratmangun/tuya-ai-doorbell"
                size={28}
                delay={30}
                color="rgba(255,255,255,0.8)"
            />
        </AbsoluteFill>
    );
};

// ============== MAIN COMPOSITION ==============
export const SmartDoorbellExplainer: React.FC = () => {
    return (
        <AbsoluteFill style={{ backgroundColor: COLORS.dark }}>
            {/* Scene 1: Title (0-5 sec) */}
            <Sequence from={0} durationInFrames={150}>
                <TitleScene />
            </Sequence>

            {/* Scene 2: Problem (5-12 sec) */}
            <Sequence from={150} durationInFrames={210}>
                <ProblemScene />
            </Sequence>

            {/* Scene 3: Solution Intro (12-18 sec) */}
            <Sequence from={360} durationInFrames={180}>
                <SolutionScene />
            </Sequence>

            {/* Scene 4: Features (18-28 sec) */}
            <Sequence from={540} durationInFrames={300}>
                <FeaturesScene1 />
            </Sequence>

            {/* Scene 5: How It Works (28-40 sec) */}
            <Sequence from={840} durationInFrames={360}>
                <HowItWorksScene />
            </Sequence>

            {/* Scene 6: Tech Stack (40-47 sec) */}
            <Sequence from={1200} durationInFrames={210}>
                <TechStackScene />
            </Sequence>

            {/* Scene 7: CTA (47-50 sec) */}
            <Sequence from={1410} durationInFrames={90}>
                <CTAScene />
            </Sequence>
        </AbsoluteFill>
    );
};
