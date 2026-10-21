package com.ysmod.hook;

import android.app.Activity;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.CompoundButton;
import android.widget.LinearLayout;
import android.widget.Switch;
import android.widget.TextView;

/** 模块设置页：开关模型替换。改完需要重启原神生效。 */
public class SettingsActivity extends Activity {

    private static final String PREF = "ysmod";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        int pad = (int) (getResources().getDisplayMetrics().density * 20);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("ysmod 模型替换");
        title.setTextSize(20);
        root.addView(title, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        TextView desc = new TextView(this);
        desc.setText("\n开启后，进入游戏会自动把角色模型替换成 mod 资源。\n修改后需要重启原神才会生效。");
        desc.setTextSize(13);
        root.addView(desc, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        final SharedPreferences sp = getSharedPreferences(PREF, MODE_PRIVATE);

        Switch sw = new Switch(this);
        sw.setText("替换模型");
        sw.setChecked(sp.getBoolean("replace", true));
        sw.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton b, boolean checked) {
                sp.edit().putBoolean("replace", checked).apply();
            }
        });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = pad;
        root.addView(sw, lp);

        TextView state = new TextView(this);
        state.setText("当前状态：" + (sw.isChecked() ? "开启" : "关闭"));
        state.setGravity(Gravity.START);
        sw.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton b, boolean checked) {
                sp.edit().putBoolean("replace", checked).apply();
                state.setText("当前状态：" + (checked ? "开启" : "关闭"));
            }
        });
        root.addView(state, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        setContentView(root);
    }
}
