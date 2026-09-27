package com.satori.wx.ui;

import android.graphics.Path;

/**
 * M3E 形状库里「饼干」「花瓣」「爆炸」这一族圆润多瓣形的极坐标近似，以及它们之间的连续形变。
 *
 * <p>每个形状是 r(θ) = R · (1 + a·cos(nθ))：n 是瓣数，a 是起伏。两个形状按 r 逐点插值，
 * 所以任意两种形状之间的形变都连续、不跳变——这正是 M3E 形状形变（shape morph）的要求。
 * 只画装饰：语义永远由旁边的文字承担。
 */
final class Morph {
    private Morph() {}

    /** 一种形状：瓣数 + 起伏。 */
    static final class Spec {
        final int lobes;
        final float amplitude;

        Spec(int lobes, float amplitude) {
            this.lobes = lobes;
            this.amplitude = amplitude;
        }
    }

    static final Spec CIRCLE = new Spec(0, 0f);
    static final Spec COOKIE_9 = new Spec(9, 0.07f);
    static final Spec COOKIE_4 = new Spec(4, 0.11f);
    static final Spec SOFT_BURST = new Spec(10, 0.08f);
    static final Spec SUNNY = new Spec(8, 0.05f);
    static final Spec PENTAGON = new Spec(5, 0.09f);
    static final Spec OVAL = new Spec(2, 0.10f);
    static final Spec PILL = new Spec(2, 0.17f);

    /** M3E 加载指示器依次经过的七种形状。 */
    static final Spec[] LOADING = {SOFT_BURST, COOKIE_9, PENTAGON, PILL, SUNNY, COOKIE_4, OVAL};

    private static final int STEPS = 120;

    /**
     * 把 a 与 b 之间进度 f 的形状写进 path。
     *
     * @param rotation 整体旋转（弧度）
     */
    static void fill(Path path, float cx, float cy, float radius, Spec a, Spec b, float f, float rotation) {
        path.reset();
        for (int i = 0; i <= STEPS; i++) {
            double theta = Math.PI * 2 * i / STEPS;
            double ra = 1 + a.amplitude * Math.cos(a.lobes * theta);
            double rb = 1 + b.amplitude * Math.cos(b.lobes * theta);
            double r = radius * (ra + (rb - ra) * f);
            float x = cx + (float) (r * Math.cos(theta + rotation));
            float y = cy + (float) (r * Math.sin(theta + rotation));
            if (i == 0) path.moveTo(x, y);
            else path.lineTo(x, y);
        }
        path.close();
    }
}
