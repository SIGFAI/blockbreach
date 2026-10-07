package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.WorldBridge;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.projectile.arrow.AbstractArrow;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Redirect;

/**
 * An arrow also checks every tick whether it is inside a block's shape and sticks there: inside one of the host's
 * barriers (see BarrierProjectileMixin) Steve's arrow keeps flying. Where it really hits is the host's call ({"t":"projhit"}).
 */
@Mixin(AbstractArrow.class)
abstract class ArrowBarrierMixin {
	@Redirect(method = "tick", at = @At(value = "INVOKE",
		target = "Lnet/minecraft/world/level/block/state/BlockState;getCollisionShape(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/phys/shapes/VoxelShape;"))
	private VoxelShape passthrough$notStuckInBarriers(final BlockState state, final BlockGetter level, final BlockPos pos) {
		return state.is(Blocks.BARRIER) && WorldBridge.hostTraced((AbstractArrow) (Object) this) ? Shapes.empty() : state.getCollisionShape(level, pos);
	}
}
