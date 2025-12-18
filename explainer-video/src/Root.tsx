import { Composition } from "remotion";
import { SmartDoorbellExplainer } from "./SmartDoorbellExplainer";

// 60 seconds at 30fps = 1800 frames
// We'll use 50 seconds to leave room = 1500 frames
export const RemotionRoot: React.FC = () => {
    return (
        <>
            <Composition
                id="SmartDoorbellExplainer"
                component={SmartDoorbellExplainer}
                durationInFrames={1500}
                fps={30}
                width={1920}
                height={1080}
            />
        </>
    );
};
